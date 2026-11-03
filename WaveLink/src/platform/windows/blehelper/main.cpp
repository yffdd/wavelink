// Windows-only BLE host. stdout is a versioned, length-prefixed UTF-8 JSON
// protocol; stderr is diagnostic-only. No Qt/MSVC ABI crosses the boundary.
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "../../../hardware/ble/blesendpolicy.h"

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Data::Json;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::Advertisement;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Devices::Radios;
using namespace Windows::Security::Cryptography;
using namespace Windows::Storage::Streams;

namespace {
constexpr uint32_t MaxMessage = 1024 * 1024;
constexpr size_t MaxOutput = 4 * 1024 * 1024;
constexpr UINT CommandMessage = WM_APP + 1;
std::mutex outputMutex, commandMutex;
std::condition_variable outputReady;
std::deque<std::string> outgoingFrames;
std::deque<std::string> commands;
size_t outputBytes = 0;
HWND commandWindow = nullptr;

JsonObject event(const wchar_t *type, const hstring &session = L"0", const hstring &request = L"0")
{
    JsonObject obj;
    obj.SetNamedValue(L"event", JsonValue::CreateStringValue(type));
    obj.SetNamedValue(L"session", JsonValue::CreateStringValue(session));
    obj.SetNamedValue(L"request", JsonValue::CreateStringValue(request));
    return obj;
}
void str(JsonObject &obj, const wchar_t *key, const hstring &value)
{ obj.SetNamedValue(key, JsonValue::CreateStringValue(value)); }
void num(JsonObject &obj, const wchar_t *key, double value)
{ obj.SetNamedValue(key, JsonValue::CreateNumberValue(value)); }
void flag(JsonObject &obj, const wchar_t *key, bool value)
{ obj.SetNamedValue(key, JsonValue::CreateBooleanValue(value)); }
void emit(JsonObject const &obj)
{
    auto bytes = to_string(obj.Stringify());
    std::lock_guard<std::mutex> lock(outputMutex);
    // A blocked consumer must never grow the acquisition queue indefinitely.
    // The client treats an unexpected exit as a data-integrity failure.
    if (bytes.size() > MaxMessage || outputBytes + bytes.size() + 4 > MaxOutput)
        ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
    outputBytes += bytes.size() + 4;
    outgoingFrames.push_back(std::move(bytes));
    outputReady.notify_one();
}
bool transfer(HANDLE pipe, void *data, uint32_t length, bool writing)
{
    auto p = static_cast<unsigned char *>(data);
    while (length) {
        DWORD done = 0;
        const bool ok = writing ? WriteFile(pipe, p, length, &done, nullptr)
                                : ReadFile(pipe, p, length, &done, nullptr);
        if (!ok || done == 0) return false;
        p += done;
        length -= done;
    }
    return true;
}
void error(const hstring &session, const hstring &request, const hstring &phase,
           const hstring &message, int32_t code = 0, bool fatal = true)
{
    auto obj = event(L"error", session, request);
    str(obj, L"phase", phase); str(obj, L"message", message);
    num(obj, L"code", code); flag(obj, L"fatal", fatal);
    emit(obj);
}
bool has(GattCharacteristic c, GattCharacteristicProperties property)
{
    return (static_cast<uint32_t>(c.CharacteristicProperties()) & static_cast<uint32_t>(property)) != 0;
}
void checkStatus(GattCommunicationStatus status)
{
    if (status != GattCommunicationStatus::Success)
        throw hresult_error(E_FAIL, L"GATT operation failed (status " + to_hstring(static_cast<int>(status)) + L"). Check range, pairing and permissions.");
}
struct Session {
    hstring id;
    BluetoothLEDevice device{nullptr};
    std::vector<GattDeviceService> services;
    std::map<uint16_t, GattCharacteristic> characteristics;
    GattCharacteristic rx{nullptr}, tx{nullptr};
    event_token valueToken{}, connectionToken{};
    bool valueRegistered = false, connectionRegistered = false;
    std::atomic<bool> ready{false};
    bool busy = false; // Only accessed on the STA command thread.
    GattWriteOption writeOption = GattWriteOption::WriteWithResponse;
    GattSession gattSession{nullptr};
    event_token pduToken{}; bool pduRegistered = false;
    std::mutex sendMutex; // MTU callbacks run outside the STA command thread.
    int mtu = 0, packetSize = 20, effectivePacket = 20, intervalMs = 10;
    uint64_t capabilityRevision = 1; bool sendBlocked = false;
    ~Session() {
        try {
            if (pduRegistered) gattSession.MaxPduSizeChanged(pduToken);
            if (valueRegistered) rx.ValueChanged(valueToken);
            if (connectionRegistered) device.ConnectionStatusChanged(connectionToken);
            for (auto const &s : services) s.Close();
            if (device) device.Close();
        } catch (...) {}
    }
};
std::shared_ptr<Session> current;

// Caller holds sendMutex, including while queueing the snapshot. Therefore
// asynchronous MTU changes and settings acknowledgements have a total order.
void addSendSettings(JsonObject &obj, Session const &s)
{
    JsonObject settings;
    str(settings, L"revision", to_hstring(s.capabilityRevision));
    num(settings, L"mtu", s.mtu); num(settings, L"packetSize", s.packetSize);
    num(settings, L"effectivePacket", s.effectivePacket); num(settings, L"intervalMs", s.intervalMs);
    flag(settings, L"blocked", s.sendBlocked); obj.SetNamedValue(L"sendSettings", settings);
}
void setSendSettings(JsonObject const &command, Session &s)
{
    const double packet = command.GetNamedNumber(L"packetSize", 20), interval = command.GetNamedNumber(L"intervalMs", 10);
    if (packet < 1 || packet > 512 || interval < 0 || interval > 1000 || packet != int(packet) || interval != int(interval))
        throw hresult_error(E_INVALIDARG, L"Packet size must be an integer in 1-512 bytes and interval in 0-1000 ms.");
    if (BleSendPolicy::needsConfirmation(int(packet), int(interval)) && !command.GetNamedBoolean(L"advancedConfirmed", false))
        throw hresult_error(E_INVALIDARG, L"Faster BLE sending requires explicit confirmation of module/application capacity.");
    s.packetSize = int(packet); s.intervalMs = int(interval);
    s.effectivePacket = BleSendPolicy::effective(s.packetSize, s.mtu);
    s.sendBlocked = false; ++s.capabilityRevision;
}
void observePdu(std::shared_ptr<Session> const &s)
{
    // Reading MTU is optional (older Windows/driver may not expose Session).
    // Failure means unknown/20 bytes, never a guessed large packet.
    try {
        s->gattSession = s->tx.Service().Session();
        std::weak_ptr<Session> weak = s;
        s->pduToken = s->gattSession.MaxPduSizeChanged([weak](auto const &sender, auto const &) {
            try {
                if (auto session = weak.lock()) {
                    int mtu = 0;
                    try { mtu = sender.MaxPduSize(); } catch (...) {} // Optional query: unknown, not an RX failure.
                    std::lock_guard<std::mutex> lock(session->sendMutex);
                    const int known = mtu >= 23 ? mtu : 0;
                    if (known == session->mtu) return;
                    session->mtu = known; ++session->capabilityRevision;
                    if (session->effectivePacket > BleSendPolicy::limit(known)) session->sendBlocked = true;
                    auto obj = event(L"capabilities", session->id); addSendSettings(obj, *session); emit(obj);
                }
            } catch (...) { ExitProcess(ERROR_INVALID_DATA); }
        });
        s->pduRegistered = true;
        std::lock_guard<std::mutex> lock(s->sendMutex);
        const int mtu = s->gattSession.MaxPduSize(); s->mtu = mtu >= 23 ? mtu : 0;
    } catch (...) { std::lock_guard<std::mutex> lock(s->sendMutex); s->mtu = 0; }
}
BluetoothLEAdvertisementWatcher watcher{nullptr};
std::atomic<uint64_t> scanGeneration{0};
std::mutex scanMutex;
std::map<uint64_t, ULONGLONG> lastAdvertisements;

void stopScan()
{
    ++scanGeneration;
    if (watcher) { watcher.Stop(); watcher = nullptr; }
}
fire_and_forget execute(JsonObject command)
{
    const auto sessionId = command.GetNamedString(L"session", L"0");
    const auto requestId = command.GetNamedString(L"request", L"0");
    const auto operation = command.GetNamedString(L"op", L"");
    std::shared_ptr<Session> session;
    bool ownsBusy = false;
    try {
        if (operation == L"probe" || operation == L"scan") {
            const auto generation = ++scanGeneration;
            auto adapter = co_await BluetoothAdapter::GetDefaultAsync();
            if (generation != scanGeneration) co_return;
            if (!adapter || !adapter.IsLowEnergySupported())
                throw hresult_error(E_FAIL, L"No Bluetooth Low Energy adapter is available.");
            auto radio = co_await adapter.GetRadioAsync();
            if (generation != scanGeneration) co_return;
            if (radio && radio.State() != RadioState::On)
                throw hresult_error(E_FAIL, L"Bluetooth is turned off. Enable it in Windows settings.");
            if (operation == L"probe") {
                auto obj = event(L"adapter", sessionId, requestId);
                str(obj, L"name", radio ? radio.Name() : L"Bluetooth LE"); emit(obj);
                co_return;
            }
            if (current) throw hresult_error(E_ILLEGAL_METHOD_CALL, L"Disconnect before scanning.");
            if (watcher) watcher.Stop();
            watcher = BluetoothLEAdvertisementWatcher();
            watcher.ScanningMode(BluetoothLEScanningMode::Active);
            { std::lock_guard<std::mutex> lock(scanMutex); lastAdvertisements.clear(); }
            watcher.Received([generation, sessionId, requestId](auto const &, BluetoothLEAdvertisementReceivedEventArgs const &args) {
                try {
                    if (generation != scanGeneration) return;
                    const auto now = GetTickCount64();
                    { std::lock_guard<std::mutex> lock(scanMutex);
                      auto &previous = lastAdvertisements[args.BluetoothAddress()];
                      if (now - previous < 250) return;
                      previous = now;
                      if (lastAdvertisements.size() > 4096) lastAdvertisements.erase(lastAdvertisements.begin()); }
                    auto obj = event(L"device", sessionId, requestId);
                    str(obj, L"address", to_hstring(args.BluetoothAddress()));
                    str(obj, L"name", args.Advertisement().LocalName());
                    num(obj, L"rssi", args.RawSignalStrengthInDBm());
                    num(obj, L"addressType", static_cast<int>(args.BluetoothAddressType()));
                    flag(obj, L"connectable", args.IsConnectable());
                    emit(obj);
                } catch (...) {}
            });
            watcher.Stopped([generation, sessionId, requestId](auto const &, BluetoothLEAdvertisementWatcherStoppedEventArgs const &args) {
                if (generation != scanGeneration) return;
                if (args.Error() != BluetoothError::Success)
                    error(sessionId, requestId, L"scan", L"Bluetooth scan stopped with system error.", static_cast<int>(args.Error()));
            });
            watcher.Start();
            emit(event(L"scanning", sessionId, requestId));
        } else if (operation == L"stopScan") {
            stopScan(); emit(event(L"scanStopped", sessionId, requestId));
        } else if (operation == L"connect") {
            stopScan();
            if (current) throw hresult_error(E_ILLEGAL_METHOD_CALL, L"A connection already exists.");
            session = std::make_shared<Session>(); session->id = sessionId; current = session;
            const auto address = std::stoull(to_string(command.GetNamedString(L"address")));
            auto addressType = static_cast<BluetoothAddressType>(static_cast<int>(command.GetNamedNumber(L"addressType", 0)));
            session->device = co_await BluetoothLEDevice::FromBluetoothAddressAsync(address, addressType);
            if (!session->device) throw hresult_error(E_ACCESSDENIED, L"Cannot access the BLE device. Check pairing, permissions and range.");
            auto progress = event(L"discovering", sessionId, requestId); emit(progress);
            auto services = co_await session->device.GetGattServicesAsync(BluetoothCacheMode::Uncached);
            checkStatus(services.Status());
            JsonArray entries;
            for (auto const &service : services.Services()) {
                session->services.push_back(service);
                auto result = co_await service.GetCharacteristicsAsync(BluetoothCacheMode::Uncached);
                if (result.Status() != GattCommunicationStatus::Success) {
                    auto inaccessible = JsonObject();
                    str(inaccessible, L"service", to_hstring(service.Uuid()));
                    num(inaccessible, L"serviceHandle", service.AttributeHandle());
                    num(inaccessible, L"discoveryStatus", static_cast<int>(result.Status()));
                    entries.Append(inaccessible); continue;
                }
                for (auto const &c : result.Characteristics()) {
                    session->characteristics.emplace(c.AttributeHandle(), c);
                    JsonObject entry;
                    str(entry, L"service", to_hstring(service.Uuid())); str(entry, L"uuid", to_hstring(c.Uuid()));
                    num(entry, L"serviceHandle", service.AttributeHandle()); num(entry, L"handle", c.AttributeHandle());
                    num(entry, L"properties", static_cast<uint32_t>(c.CharacteristicProperties()));
                    entries.Append(entry);
                }
            }
            if (session->characteristics.empty()) throw hresult_error(E_FAIL, L"No accessible GATT characteristics were found.");
            std::weak_ptr<Session> weak = session;
            session->connectionToken = session->device.ConnectionStatusChanged([weak](auto const &device, auto const &) {
                if (auto s = weak.lock(); s && s->ready && device.ConnectionStatus() == BluetoothConnectionStatus::Disconnected) {
                    s->ready = false;
                    error(s->id, L"0", L"connection", L"The BLE device disconnected. Check power and distance.");
                }
            });
            session->connectionRegistered = true;
            auto obj = event(L"services", sessionId, requestId);
            obj.SetNamedValue(L"characteristics", entries);
            str(obj, L"name", session->device.Name());
            flag(obj, L"paired", session->device.DeviceInformation().Pairing().IsPaired()); emit(obj);
        } else if (operation == L"configure" || operation == L"write" || operation == L"read" || operation == L"sendSettings") {
            session = current;
            if (!session || session->id != sessionId) co_return;
            if (session->busy) throw hresult_error(E_ILLEGAL_METHOD_CALL, L"Another GATT operation is in progress.");
            session->busy = true; ownsBusy = true;
            if (operation == L"configure") {
                if (session->ready) throw hresult_error(E_ILLEGAL_METHOD_CALL, L"Reconnect to change the data channel.");
                const int rx = static_cast<int>(command.GetNamedNumber(L"rx", 0));
                const int tx = static_cast<int>(command.GetNamedNumber(L"tx", 0));
                if (rx == 0 && tx == 0) throw hresult_error(E_INVALIDARG, L"Select at least one receive or transmit characteristic.");
                if (rx) {
                    session->rx = session->characteristics.at(static_cast<uint16_t>(rx));
                    if (!has(session->rx, GattCharacteristicProperties::Notify) && !has(session->rx, GattCharacteristicProperties::Indicate))
                        throw hresult_error(E_INVALIDARG, L"The receive characteristic does not support Notify or Indicate.");
                    std::weak_ptr<Session> weak = session;
                    session->valueToken = session->rx.ValueChanged([weak](auto const &, GattValueChangedEventArgs const &args) {
                        try {
                            if (auto s = weak.lock()) {
                                // Keep pre-ready notifications for the client to count/ignore explicitly.
                                auto obj = event(L"data", s->id);
                                str(obj, L"data", CryptographicBuffer::EncodeToBase64String(args.CharacteristicValue()));
                                str(obj, L"timestamp100ns", to_hstring(args.Timestamp().time_since_epoch().count()));
                                emit(obj);
                            }
                        } catch (...) { ExitProcess(ERROR_INVALID_DATA); }
                    });
                    session->valueRegistered = true;
                    const auto mode = has(session->rx, GattCharacteristicProperties::Notify)
                        ? GattClientCharacteristicConfigurationDescriptorValue::Notify
                        : GattClientCharacteristicConfigurationDescriptorValue::Indicate;
                    checkStatus(co_await session->rx.WriteClientCharacteristicConfigurationDescriptorAsync(mode));
                }
                if (tx) {
                    session->tx = session->characteristics.at(static_cast<uint16_t>(tx));
                    const bool withoutResponse = command.GetNamedBoolean(L"withoutResponse", false);
                    if (!has(session->tx, withoutResponse ? GattCharacteristicProperties::WriteWithoutResponse : GattCharacteristicProperties::Write))
                        throw hresult_error(E_INVALIDARG, L"The requested write mode is not supported by the selected characteristic.");
                    session->writeOption = withoutResponse ? GattWriteOption::WriteWithoutResponse : GattWriteOption::WriteWithResponse;
                    observePdu(session);
                }
                session->ready = true;
                if (session->device.ConnectionStatus() != BluetoothConnectionStatus::Connected) {
                    session->ready = false;
                    throw hresult_error(E_FAIL, L"The BLE connection was lost while preparing the data channel.");
                }
                std::lock_guard<std::mutex> lock(session->sendMutex);
                setSendSettings(command, *session);
                auto obj = event(L"ready", sessionId, requestId); flag(obj, L"canSend", tx != 0); flag(obj, L"canReceive", rx != 0);
                addSendSettings(obj, *session); emit(obj);
            } else if (operation == L"sendSettings") {
                if (!session->ready || !session->tx) throw hresult_error(E_ILLEGAL_METHOD_CALL, L"No transmit channel is ready.");
                std::lock_guard<std::mutex> lock(session->sendMutex);
                setSendSettings(command, *session);
                auto obj = event(L"sendSettingsApplied", sessionId, requestId); addSendSettings(obj, *session); emit(obj);
            } else if (operation == L"write") {
                if (!session->ready || !session->tx) throw hresult_error(E_ILLEGAL_METHOD_CALL, L"No transmit channel is ready.");
                auto bytes = CryptographicBuffer::DecodeFromBase64String(command.GetNamedString(L"data"));
                if (bytes.Length() == 0) throw hresult_error(E_INVALIDARG, L"Empty BLE packets are not supported.");
                {
                    std::lock_guard<std::mutex> lock(session->sendMutex);
                    // Check again in the native backend before submission. A
                    // capability event may not have reached the Qt UI yet.
                    if (session->sendBlocked || bytes.Length() > uint32_t(session->effectivePacket) || bytes.Length() > uint32_t(BleSendPolicy::limit(session->mtu))) {
                        session->sendBlocked = true; ++session->capabilityRevision;
                        auto obj = event(L"writeRejected", sessionId, requestId); addSendSettings(obj, *session); emit(obj);
                        session->busy = false; co_return;
                    }
                }
                auto result = co_await session->tx.WriteValueWithResultAsync(bytes, session->writeOption);
                checkStatus(result.Status());
                auto obj = event(L"written", sessionId, requestId); num(obj, L"bytes", bytes.Length()); emit(obj);
            } else {
                const auto handle = static_cast<uint16_t>(command.GetNamedNumber(L"handle"));
                auto c = session->characteristics.at(handle);
                if (!has(c, GattCharacteristicProperties::Read)) throw hresult_error(E_INVALIDARG, L"This characteristic is not readable.");
                auto result = co_await c.ReadValueAsync(BluetoothCacheMode::Uncached);
                checkStatus(result.Status());
                auto obj = event(L"read", sessionId, requestId); num(obj, L"handle", handle);
                str(obj, L"data", CryptographicBuffer::EncodeToBase64String(result.Value())); emit(obj);
            }
            session->busy = false;
        } else if (operation == L"shutdown") {
            stopScan(); PostQuitMessage(0);
        } else {
            throw hresult_error(E_INVALIDARG, L"Unknown BLE host operation.");
        }
    } catch (hresult_error const &e) {
        if (ownsBusy) session->busy = false;
        error(sessionId, requestId, operation, e.message(), e.code(), operation != L"read" && operation != L"sendSettings");
    } catch (std::exception const &e) {
        if (ownsBusy) session->busy = false;
        error(sessionId, requestId, operation, to_hstring(e.what()), E_INVALIDARG, operation != L"read" && operation != L"sendSettings");
    } catch (...) {
        if (ownsBusy) session->busy = false;
        error(sessionId, requestId, operation, L"Unexpected BLE backend failure.", E_FAIL);
    }
}
LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == CommandMessage) {
        std::deque<std::string> batch;
        { std::lock_guard<std::mutex> lock(commandMutex); batch.swap(commands); }
        for (auto const &bytes : batch) {
            try { execute(JsonObject::Parse(to_hstring(bytes))); }
            catch (...) { error(L"0", L"0", L"ipc", L"Invalid JSON command.", E_INVALIDARG); }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
}

int runFileReader();
int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--file-reader") return runFileReader();
    if (argc != 1) return ERROR_INVALID_PARAMETER;
    try {
        // STA + message pump keeps consent-sensitive WinRT calls on a desktop
        // UI-capable thread. Pipe IO and notification callbacks never block it.
        init_apartment(apartment_type::single_threaded);
        WNDCLASSW cls{}; cls.lpfnWndProc = windowProc;
        cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"WaveLinkBleHost";
        RegisterClassW(&cls);
        commandWindow = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
        if (!commandWindow) return ERROR_INVALID_WINDOW_HANDLE;
        std::thread([] {
            const auto pipe = GetStdHandle(STD_OUTPUT_HANDLE);
            for (;;) {
                std::string bytes;
                { std::unique_lock<std::mutex> lock(outputMutex);
                  outputReady.wait(lock, [] { return !outgoingFrames.empty(); });
                  bytes = std::move(outgoingFrames.front()); outgoingFrames.pop_front(); outputBytes -= bytes.size() + 4; }
                uint32_t length = static_cast<uint32_t>(bytes.size());
                if (!transfer(pipe, &length, 4, true) || !transfer(pipe, bytes.data(), length, true)) ExitProcess(ERROR_BROKEN_PIPE);
            }
        }).detach();
        std::thread([] {
            const auto pipe = GetStdHandle(STD_INPUT_HANDLE);
            for (;;) {
                uint32_t length = 0;
                if (!transfer(pipe, &length, 4, false)) { PostMessageW(commandWindow, WM_CLOSE, 0, 0); ExitProcess(0); }
                if (!length || length > MaxMessage) ExitProcess(ERROR_INVALID_DATA);
                std::string bytes(length, '\0');
                if (!transfer(pipe, bytes.data(), length, false)) ExitProcess(ERROR_BROKEN_PIPE);
                { std::lock_guard<std::mutex> lock(commandMutex);
                  if (commands.size() >= 64) ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
                  commands.push_back(std::move(bytes)); }
                PostMessageW(commandWindow, CommandMessage, 0, 0);
            }
        }).detach();
        JsonObject hello;
        str(hello, L"event", L"hello"); num(hello, L"version", 1);
        str(hello, L"role", L"ble"); num(hello, L"fileReaderVersion", 1); num(hello, L"sendControlVersion", 1); emit(hello);
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        current.reset();
        // No joining threads blocked on pipe IO during cancellation.
        ExitProcess(0);
    } catch (hresult_error const &e) {
        return static_cast<int>(e.code());
    }
}
