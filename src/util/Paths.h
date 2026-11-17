#pragma once

#include <string>

namespace proxy {

// Каталог, от которого считаются относительные пути в конфигурации. Пустой
// каталог означает «не привязан»: пути остаются как записаны, и их разрешает
// вызывающая сторона (так ведут себя отдельные конфиги в тестах).
std::string anchorPath(const std::string& path, const std::string& base_dir);

// Абсолютный путь к каталогу, содержащему файл. Файл может быть относительным:
// каталог всё равно получится абсолютным.
std::string directoryOf(const std::string& file);

} // namespace proxy