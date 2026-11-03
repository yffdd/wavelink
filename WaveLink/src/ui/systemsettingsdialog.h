#pragma once
#include "systempreferences.h"
#include <QDialog>
#include <functional>

class SystemSettingsDialog final : public QDialog {
public:
    enum class Category { Appearance, Receive, Plot, Updates };
    SystemSettingsDialog(const SystemPreferences &preferences,
                         const std::function<void(const SystemPreferences &)> &apply, QWidget *parent = nullptr,
                         Category initialCategory = Category::Appearance);
};
