#pragma once

#include <string>
#include <vector>

namespace proxy {

struct ServiceTarget {
    const char* key;
    const char* label;
    const char* unit;
};

// Таблица сервисов панели: ключ, подпись, unit. Меню панели и API читают её, а
// не повторяют ключи и подписи литералами.
const std::vector<ServiceTarget>& serviceTargets();
// Перезапуск самого прокси отложен: ответ клиенту обязан уйти до остановки.
bool deferredRestart(const std::string& key);
const ServiceTarget* findServiceTarget(const std::string& key);
bool systemUnitInstalled(const std::string& unit);
std::vector<std::string> restartServiceCommand(const std::string& unit);

} // namespace proxy