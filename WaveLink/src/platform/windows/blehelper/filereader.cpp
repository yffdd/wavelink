// Isolated source-file reader. This mode never initializes Bluetooth.
// The pipe control thread remains available while the IO worker is blocked:
// parent EOF or cancel exits only this reader process, including its worker.
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <limits>

using namespace winrt;
using namespace Windows::Data::Json;
using namespace Windows::Security::Cryptography;
namespace {
constexpr uint32_t MaxMessage = 128 * 1024;
std::mutex queueMutex;
std::condition_variable queueReady;
std::deque<std::string> requests;

bool transfer(HANDLE pipe, void *data, uint32_t size, bool writing)
{
    auto ptr = static_cast<unsigned char *>(data);
    while (size) {
        DWORD done = 0;
        if (!(writing ? WriteFile(pipe, ptr, size, &done, nullptr) : ReadFile(pipe, ptr, size, &done, nullptr)) || !done)
            return false;
        ptr += done; size -= done;
    }
    return true;
}
void put(JsonObject &obj, const wchar_t *key, const hstring &value)
{ obj.SetNamedValue(key, JsonValue::CreateStringValue(value)); }
void output(JsonObject const &obj)
{
    auto bytes = to_string(obj.Stringify());
    uint32_t length = static_cast<uint32_t>(bytes.size());
    if (!length || length > MaxMessage || !transfer(GetStdHandle(STD_OUTPUT_HANDLE), &length, 4, true) ||
        !transfer(GetStdHandle(STD_OUTPUT_HANDLE), bytes.data(), length, true)) ExitProcess(ERROR_BROKEN_PIPE);
}
int64_t number(hstring const &text)
{
    if (text.empty()) throw hresult_invalid_argument(L"Missing file offset.");
    uint64_t result = 0;
    for (auto ch : text) {
        if (ch < L'0' || ch > L'9' || result > (uint64_t(std::numeric_limits<int64_t>::max()) - (ch - L'0')) / 10)
            throw hresult_invalid_argument(L"Invalid file offset.");
        result = result * 10 + (ch - L'0');
    }
    return static_cast<int64_t>(result);
}
struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
void worker()
{
    init_apartment(apartment_type::multi_threaded);
    FileHandle file;
    hstring session, transferId;
    int64_t total = -1, offset = 0;
    for (;;) {
        std::string bytes;
        { std::unique_lock<std::mutex> lock(queueMutex);
          queueReady.wait(lock, [] { return !requests.empty(); });
          bytes = std::move(requests.front()); requests.pop_front(); }
        JsonObject req, result;
        try {
            req = JsonObject::Parse(to_hstring(bytes));
            put(result, L"session", req.GetNamedString(L"session"));
            put(result, L"transfer", req.GetNamedString(L"transfer"));
            put(result, L"request", req.GetNamedString(L"request"));
            auto op = req.GetNamedString(L"op");
            if (op == L"open") {
                if (file.value != INVALID_HANDLE_VALUE) throw hresult_invalid_argument(L"Only one source file is allowed per reader.");
                const auto path = req.GetNamedString(L"path");
                if (path.empty() || path.size() > 32767 || std::wstring(path).find(L'\0') != std::wstring::npos)
                    throw hresult_invalid_argument(L"Invalid source path.");
                // No concurrent writers/deleters while this handle exists.
                file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                if (file.value == INVALID_HANDLE_VALUE) throw_last_error();
                if (GetFileType(file.value) != FILE_TYPE_DISK) throw hresult_invalid_argument(L"Only regular disk files can be sent.");
                BY_HANDLE_FILE_INFORMATION info{};
                check_bool(GetFileInformationByHandle(file.value, &info));
                if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) throw hresult_invalid_argument(L"A folder cannot be sent.");
                LARGE_INTEGER size{}; check_bool(GetFileSizeEx(file.value, &size));
                if (size.QuadPart < 0) throw hresult_invalid_argument(L"Invalid file size.");
                total = size.QuadPart; offset = 0;
                session = req.GetNamedString(L"session"); transferId = req.GetNamedString(L"transfer");
                put(result, L"event", L"opened"); put(result, L"size", to_hstring(total));
            } else if (op == L"read") {
                if (file.value == INVALID_HANDLE_VALUE || req.GetNamedString(L"session") != session ||
                    req.GetNamedString(L"transfer") != transferId) throw hresult_invalid_argument(L"Source task identity mismatch.");
                const int64_t requestedOffset = number(req.GetNamedString(L"offset"));
                const double requestedSize = req.GetNamedNumber(L"length");
                if (!(requestedSize >= 1 && requestedSize <= 4096) || requestedSize != static_cast<int>(requestedSize) ||
                    requestedOffset != offset || offset >= total || requestedSize > total - offset)
                    throw hresult_invalid_argument(L"Source request is out of bounds.");
                LARGE_INTEGER current{}; check_bool(GetFileSizeEx(file.value, &current));
                if (current.QuadPart != total) throw hresult_error(E_FAIL, L"Source file size changed during transmission.");
                std::vector<uint8_t> data(static_cast<size_t>(requestedSize));
                DWORD count = 0; check_bool(ReadFile(file.value, data.data(), static_cast<DWORD>(data.size()), &count, nullptr));
                if (!count) throw hresult_error(E_FAIL, L"Unexpected end of source file.");
                data.resize(count);
                put(result, L"event", L"chunk"); put(result, L"offset", to_hstring(offset));
                put(result, L"data", CryptographicBuffer::EncodeToBase64String(CryptographicBuffer::CreateFromByteArray(data)));
                offset += count;
                result.SetNamedValue(L"eof", JsonValue::CreateBooleanValue(offset == total));
            } else throw hresult_invalid_argument(L"Unsupported source operation.");
            output(result);
        } catch (hresult_error const &e) {
            put(result, L"event", L"error"); put(result, L"message", e.message());
            result.SetNamedValue(L"code", JsonValue::CreateNumberValue(static_cast<int32_t>(e.code())));
            output(result);
            // No more IO after an error; control EOF/parent kill cleans up.
            for (;;) Sleep(INFINITE);
        } catch (...) { ExitProcess(ERROR_INVALID_DATA); }
    }
}
}
int runFileReader()
{
    try {
        init_apartment(apartment_type::multi_threaded);
        JsonObject hello;
        put(hello, L"event", L"hello"); put(hello, L"role", L"file-reader");
        hello.SetNamedValue(L"version", JsonValue::CreateNumberValue(1));
        hello.SetNamedValue(L"fileReaderVersion", JsonValue::CreateNumberValue(1));
        output(hello);
        std::thread(worker).detach();
        for (;;) {
            uint32_t length = 0;
            if (!transfer(GetStdHandle(STD_INPUT_HANDLE), &length, 4, false)) ExitProcess(0);
            if (!length || length > MaxMessage) ExitProcess(ERROR_INVALID_DATA);
            std::string bytes(length, '\0');
            if (!transfer(GetStdHandle(STD_INPUT_HANDLE), bytes.data(), length, false)) ExitProcess(ERROR_BROKEN_PIPE);
            auto req = JsonObject::Parse(to_hstring(bytes));
            if (req.GetNamedString(L"op") == L"cancel") ExitProcess(0);
            { std::lock_guard<std::mutex> lock(queueMutex);
              if (requests.size() >= 2) ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
              requests.push_back(std::move(bytes)); }
            queueReady.notify_one();
        }
    } catch (...) { ExitProcess(ERROR_INVALID_DATA); }
}
