#include "analytics/ProviderAnalytics.h"
#include "util/NanoRub.h"
#include "util/Ratio.h"
#include "util/Strings.h"
#include "util/Time.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <json/writer.h>

namespace proxy {
namespace {

struct CsvTable {
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows;
    bool ok = false;
    std::string error;
};

// Границы «первый/последний» — одно правило для группы и для сводки по модели.
void takeEarlier(std::int64_t& epoch, std::string& label, std::int64_t candidate_epoch,
                 const std::string& candidate_label) {
    if (epoch == 0 || (candidate_epoch != 0 && candidate_epoch < epoch)) {
        epoch = candidate_epoch;
        label = candidate_label;
    }
}

void takeLater(std::int64_t& epoch, std::string& label, std::int64_t candidate_epoch,
               const std::string& candidate_label) {
    if (epoch == 0 || candidate_epoch >= epoch) {
        epoch = candidate_epoch;
        label = candidate_label;
    }
}

std::vector<std::vector<std::string>> parseCsv(std::string_view text, char delimiter) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    bool any = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(c);
            }
            continue;
        }
        if (c == '"' && field.empty()) {
            quoted = true;
        } else if (c == delimiter) {
            row.push_back(std::move(field));
            field.clear();
            any = true;
        } else if (c == '\n') {
            row.push_back(std::move(field));
            field.clear();
            bool nonempty = any || (row.size() > 1) || !row.front().empty();
            if (nonempty) rows.push_back(std::move(row));
            row.clear();
            any = false;
        } else if (c != '\r') {
            field.push_back(c);
        }
    }
    if (!field.empty() || !row.empty()) {
        row.push_back(std::move(field));
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string normalizeHeader(std::string header) {
    header = trim(header);
    if (header.rfind("\xEF\xBB\xBF", 0) == 0) header.erase(0, 3);
    header = toLower(trim(header));
    if (header.rfind("created_at", 0) == 0) return "created_at";
    return header;
}

std::size_t findColumn(const std::map<std::string, std::size_t>& header,
                       const std::string& name) {
    auto it = header.find(name);
    return it == header.end() ? std::string::npos : it->second;
}

long double number(std::string_view value, std::size_t* bad_fields = nullptr) {
    std::string s = trim(value);
    if (s.empty()) return 0;
    try {
        std::size_t used = 0;
        const long double parsed = std::stold(s, &used);
        if (used != s.size()) throw std::invalid_argument("trailing characters");
        return parsed;
    } catch (...) {
        // Нечисловая ячейка — видимый дефект данных, а не ноль токенов.
        if (bad_fields) ++*bad_fields;
        return 0;
    }
}

std::int64_t integer(std::string_view value, std::size_t* bad_fields = nullptr) {
    const long double v = number(value, bad_fields);
    if (!std::isfinite(static_cast<double>(v))) return 0;
    return static_cast<std::int64_t>(std::max<long double>(0, v));
}

bool parseTimestamp(const std::string& input, std::string& date, int& hour,
                    std::string& display, std::int64_t& epoch) {
    const std::string s = trim(input);
    if (s.empty()) return false;
    std::tm tm{};
    std::istringstream ss(s);
    if (!(ss >> std::get_time(&tm, "%d-%m-%Y %H:%M:%S"))) {
        std::istringstream alt(s);
        if (!(alt >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S"))) return false;
    }
    char buf[16]{};
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm) == 0) return false;
    date = buf;
    hour = std::clamp(tm.tm_hour, 0, 23);
    display = s;
    // Источник — Europe/Moscow (UTC+3, без перехода на летнее время): храни
    // сопоставимый epoch для first/last и отдельно исходную локальную метку.
    epoch = static_cast<std::int64_t>(timegm(&tm)) - kMoscowUtcOffsetHours * 3600;
    return true;
}

char detectDelimiter(std::string_view text) {
    const auto line_end = text.find('\n');
    const std::string_view first = text.substr(0, line_end == std::string_view::npos ? text.size() : line_end);
    const auto semis = std::count(first.begin(), first.end(), ';');
    const auto commas = std::count(first.begin(), first.end(), ',');
    return semis >= commas ? ';' : ',';
}

} // namespace

ProviderAnalytics::ProviderAnalytics(AnalyticsConfig config) : config_(std::move(config)) {}

std::string ProviderAnalytics::normalizeProvider(const std::string& provider) {
    std::string p = toLower(trim(provider));
    return p.empty() ? "unknown" : p;
}

std::string ProviderAnalytics::groupKey(const std::string& model, const std::string& provider) {
    return model + "\x1f" + normalizeProvider(provider);
}

double ProviderAnalytics::percentile(std::vector<double> values, double q) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double pos = std::clamp(q, 0.0, 1.0) * static_cast<double>(values.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(pos));
    const auto hi = static_cast<std::size_t>(std::ceil(pos));
    if (lo == hi) return values[lo];
    const double frac = pos - static_cast<double>(lo);
    return values[lo] * (1.0 - frac) + values[hi] * frac;
}

void ProviderAnalytics::finalize(Stats& stats) {
    if (!stats.input_prices.empty()) {
        stats.input_per_token = static_cast<NanoRub>(std::llround(percentile(stats.input_prices, 0.5)));
    } else if (stats.input_tokens > stats.cached_tokens && stats.input_cost > 0) {
        stats.input_per_token = static_cast<NanoRub>(std::llround(
            static_cast<long double>(stats.input_cost) /
            static_cast<long double>(stats.input_tokens - stats.cached_tokens)));
    }
    if (!stats.cache_prices.empty()) {
        stats.cache_read_per_token =
            static_cast<NanoRub>(std::llround(percentile(stats.cache_prices, 0.5)));
    } else if (stats.cached_tokens > 0 && stats.cache_cost > 0) {
        stats.cache_read_per_token = static_cast<NanoRub>(std::llround(
            static_cast<long double>(stats.cache_cost) /
            static_cast<long double>(stats.cached_tokens)));
    }
    if (!stats.output_prices.empty()) {
        stats.output_per_token =
            static_cast<NanoRub>(std::llround(percentile(stats.output_prices, 0.5)));
    } else if (stats.output_tokens > 0 && stats.output_cost > 0) {
        stats.output_per_token = static_cast<NanoRub>(std::llround(
            static_cast<long double>(stats.output_cost) /
            static_cast<long double>(stats.output_tokens)));
    }
    stats.p50_latency_ms = percentile(stats.latencies_ms, 0.5);
    stats.p90_latency_ms = percentile(stats.latencies_ms, 0.9);
    const double seconds = stats.latency_sum_ms / 1000.0;
    const auto generated = stats.output_tokens + stats.reasoning_tokens;
    stats.tokens_per_second = seconds > 0
                                  ? static_cast<double>(stats.input_tokens + generated) / seconds
                                  : 0.0;
    stats.output_tokens_per_second =
        seconds > 0 ? static_cast<double>(generated) / seconds : 0.0;
    stats.reasoning_ratio = generated > 0
                                ? static_cast<double>(stats.reasoning_tokens) /
                                      static_cast<double>(generated)
                                : 0.0;
}

void ProviderAnalytics::accumulate(Stats& stats, const ParsedRecord& record) {
    if (stats.model.empty()) stats.model = record.model;
    if (stats.provider.empty()) stats.provider = record.provider.empty() ? "unknown" : record.provider;
    ++stats.requests;
    stats.input_tokens += record.input_tokens;
    stats.cached_tokens += record.cached_tokens;
    stats.cache_write_tokens += record.cache_write_tokens;
    stats.output_tokens += record.output_tokens;
    stats.reasoning_tokens += record.reasoning_tokens;
    stats.input_cost += record.input_cost;
    stats.cache_cost += record.cache_cost;
    stats.output_cost += record.output_cost;
    stats.total_cost += record.total_cost;
    stats.latency_sum_ms += record.latency_ms;
    stats.latency_max_ms = std::max<std::int64_t>(
        stats.latency_max_ms, static_cast<std::int64_t>(std::llround(record.latency_ms)));
    stats.max_cached_tokens = std::max(stats.max_cached_tokens, record.cached_tokens);
    takeEarlier(stats.first_epoch, stats.first_seen, record.seen_epoch, record.seen_local);
    takeLater(stats.last_epoch, stats.last_seen, record.seen_epoch, record.seen_local);
    const auto cap = static_cast<std::size_t>(std::max(1, config_.max_samples_per_group));
    auto push = [cap](std::vector<double>& values, double value) {
        if (values.size() < cap) values.push_back(value);
    };
    if (record.input_tokens > record.cached_tokens && record.input_cost > 0) {
        push(stats.input_prices, static_cast<double>(record.input_cost) /
                                     static_cast<double>(record.input_tokens - record.cached_tokens));
    }
    if (record.cached_tokens > 0 && record.cache_cost > 0) {
        push(stats.cache_prices, static_cast<double>(record.cache_cost) /
                                     static_cast<double>(record.cached_tokens));
    }
    if (record.output_tokens > 0 && record.output_cost > 0) {
        push(stats.output_prices, static_cast<double>(record.output_cost) /
                                      static_cast<double>(record.output_tokens));
    }
    push(stats.latencies_ms, record.latency_ms);

}


void ProviderAnalytics::merge(const ParsedRecord& record) {
    const std::string provider = normalizeProvider(record.provider);
    const std::string key = groupKey(record.model, provider);
    auto& group = groups_[key];
    accumulate(group, record);

    auto& time = times_[key + "\x1f" + record.date + "\x1f" + std::to_string(record.hour)];
    time.date = record.date;
    time.hour = record.hour;
    accumulate(time, record);
}

ProviderLogImportResult ProviderAnalytics::importCsvContent(std::string_view content,
                                                            const std::string& source) {
    ProviderLogImportResult result;
    if (!config_.enabled) {
        result.error = "analytics disabled";
        return result;
    }
    if (content.empty()) {
        result.error = "empty CSV";
        return result;
    }
    if (content.size() > config_.max_file_bytes) {
        result.error = "CSV exceeds max_file_bytes: " + std::to_string(content.size());
        return result;
    }
    const std::string hash = sha256Hex(content);
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (imported_hashes_.count(hash)) {
            result.ok = true;
            result.duplicate = true;
            return result;
        }
    }

    const char delimiter = detectDelimiter(content);
    auto rows = parseCsv(content, delimiter);
    if (rows.size() < 2) {
        result.error = "CSV has no data rows";
        return result;
    }
    std::map<std::string, std::size_t> header;
    for (std::size_t i = 0; i < rows.front().size(); ++i) {
        header.emplace(normalizeHeader(rows.front()[i]), i);
    }
    const auto model_col = findColumn(header, "model_name");
    const auto time_col = findColumn(header, "created_at");
    if (model_col == std::string::npos || time_col == std::string::npos) {
        result.error = "required columns model_name/created_at are missing";
        return result;
    }
    const auto provider_col = findColumn(header, "provider_name");
    const auto input_col = findColumn(header, "input_tokens");
    const auto output_col = findColumn(header, "output_tokens");
    const auto input_cost_col = findColumn(header, "input_tokens_cost");
    const auto output_cost_col = findColumn(header, "output_tokens_cost");
    const auto cached_col = findColumn(header, "input_cached_tokens");
    const auto cached_cost_col = findColumn(header, "input_cached_tokens_cost");
    const auto cache_write_col = findColumn(header, "input_cache_write_tokens");
    const auto reasoning_col = findColumn(header, "output_reasoning_tokens");
    const auto amount_col = findColumn(header, "amount");
    const auto latency_col = findColumn(header, "latency_sec");

    auto field = [&](const std::vector<std::string>& row, std::size_t col) {
        return col == std::string::npos || col >= row.size() ? std::string_view{} : std::string_view(row[col]);
    };

    std::vector<ParsedRecord> records;
    records.reserve(rows.size() - 1);
    std::size_t skipped = 0;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        if (row.empty()) continue;
        ParsedRecord r;
        r.model = trim(field(row, model_col));
        if (r.model.empty()) {
            ++skipped;
            continue;
        }
        if (!parseTimestamp(std::string(field(row, time_col)), r.date, r.hour, r.seen_local,
                            r.seen_epoch)) {
            ++skipped;
            continue;
        }
        r.provider = trim(field(row, provider_col));
        r.input_tokens = integer(field(row, input_col), &result.bad_fields);
        r.cached_tokens = integer(field(row, cached_col), &result.bad_fields);
        r.cache_write_tokens = integer(field(row, cache_write_col), &result.bad_fields);
        r.output_tokens = integer(field(row, output_col), &result.bad_fields);
        r.reasoning_tokens = integer(field(row, reasoning_col), &result.bad_fields);
        r.input_cost = rubToNano(number(field(row, input_cost_col), &result.bad_fields));
        r.cache_cost = rubToNano(number(field(row, cached_cost_col), &result.bad_fields));
        r.output_cost = rubToNano(number(field(row, output_cost_col), &result.bad_fields));
        r.latency_ms =
            static_cast<double>(number(field(row, latency_col), &result.bad_fields)) * 1000.0;
        if (r.latency_ms < 0) r.latency_ms = 0;
        const auto amount = rubToNano(number(field(row, amount_col), &result.bad_fields));
        r.total_cost = amount > 0 ? amount : (r.input_cost + r.cache_cost + r.output_cost);
        if (r.cached_tokens > r.input_tokens) r.cached_tokens = r.input_tokens;
        if (r.provider.empty() && r.input_tokens == 0 && r.output_tokens == 0 &&
            r.cached_tokens == 0 && r.cache_write_tokens == 0 && r.reasoning_tokens == 0 &&
            r.total_cost == 0) {
            ++skipped;
            continue;
        }
        records.push_back(std::move(r));
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        if (imported_hashes_.count(hash)) {
            result.ok = true;
            result.duplicate = true;
            return result;
        }
        for (const auto& record : records) merge(record);
        for (auto& [_, stats] : groups_) finalize(stats);
        for (auto& [_, stats] : times_) finalize(stats);
        imported_hashes_.insert(hash);
        ++files_;
        rows_ += records.size();
        skipped_ += skipped;
        last_error_.clear();
    }
    if (result.bad_fields > 0) {
        LOG_WARN << "provider log has non-numeric values: fields=" << result.bad_fields
                 << " source=" << source;
    }
    result.ok = true;
    result.files = 1;
    result.rows = records.size();
    result.skipped = skipped;
    return result;
}

ProviderLogImportResult ProviderAnalytics::importDirectory() {
    ProviderLogImportResult result;
    if (!config_.enabled) {
        result.error = "analytics disabled";
        return result;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(config_.provider_log_dir, ec)) {
        result.error = ec ? "provider log directory: " + ec.message()
                          : "provider log directory not found: " + config_.provider_log_dir;
        return result;
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(config_.provider_log_dir, ec)) {
        if (ec) {
            // Обрыв обхода — не «файлов нет»: частичный импорт обязан быть виден.
            result.error = "provider log directory: " + ec.message();
            return result;
        }
        if (!entry.is_regular_file()) continue;
        auto ext = toLower(entry.path().extension().string());
        if (ext == ".csv") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    result.ok = true;
    for (const auto& path : files) {
        std::error_code size_ec;
        const auto size = std::filesystem::file_size(path, size_ec);
        if (size_ec || size > config_.max_file_bytes) {
            result.ok = false;
            result.error = path.string() + ": file exceeds max_file_bytes";
            continue;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            result.error = "cannot read " + path.string();
            continue;
        }
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto one = importCsvContent(content, path.string());
        if (!one.ok) {
            result.ok = false;
            result.error = path.string() + ": " + one.error;
            continue;
        }
        ++result.files;
        result.rows += one.rows;
        result.skipped += one.skipped;
        result.bad_fields += one.bad_fields;
    }
    if (result.bad_fields > 0) {
        LOG_WARN << "provider logs have non-numeric values: fields=" << result.bad_fields;
    }
    return result;
}

void ProviderAnalytics::noteError(const std::string& what) const {
    std::lock_guard<std::mutex> lock(mu_);
    last_error_ = what;
}

bool ProviderAnalytics::saveSnapshot() const {
    if (config_.snapshot_file.empty()) return false;
    auto sampleArray = [](const std::vector<double>& values) {
        Json::Value a(Json::arrayValue);
        for (double v : values) a.append(v);
        return a;
    };
    auto writeStats = [&](const Stats& g) {
        Json::Value x(Json::objectValue);
        x["model"] = g.model;
        x["provider"] = g.provider;
        x["date"] = g.date;
        x["hour"] = g.hour;
        x["requests"] = static_cast<Json::Int64>(g.requests);
        x["input_tokens"] = static_cast<Json::Int64>(g.input_tokens);
        x["cached_tokens"] = static_cast<Json::Int64>(g.cached_tokens);
        x["cache_write_tokens"] = static_cast<Json::Int64>(g.cache_write_tokens);
        x["output_tokens"] = static_cast<Json::Int64>(g.output_tokens);
        x["reasoning_tokens"] = static_cast<Json::Int64>(g.reasoning_tokens);
        x["input_cost"] = static_cast<Json::Int64>(g.input_cost);
        x["cache_cost"] = static_cast<Json::Int64>(g.cache_cost);
        x["output_cost"] = static_cast<Json::Int64>(g.output_cost);
        x["total_cost"] = static_cast<Json::Int64>(g.total_cost);
        x["latency_sum_ms"] = g.latency_sum_ms;
        x["latency_max_ms"] = static_cast<Json::Int64>(g.latency_max_ms);
        x["max_cached_tokens"] = static_cast<Json::Int64>(g.max_cached_tokens);
        x["first_seen"] = g.first_seen;
        x["last_seen"] = g.last_seen;
        x["first_epoch"] = static_cast<Json::Int64>(g.first_epoch);
        x["last_epoch"] = static_cast<Json::Int64>(g.last_epoch);
        x["input_prices"] = sampleArray(g.input_prices);
        x["cache_prices"] = sampleArray(g.cache_prices);
        x["output_prices"] = sampleArray(g.output_prices);
        x["latencies"] = sampleArray(g.latencies_ms);
        return x;
    };
    Json::Value root(Json::objectValue);
    {
        std::lock_guard<std::mutex> lock(mu_);
        root["files"] = static_cast<Json::UInt64>(files_);
        root["rows"] = static_cast<Json::UInt64>(rows_);
        root["skipped"] = static_cast<Json::UInt64>(skipped_);
        Json::Value hashes(Json::arrayValue);
        for (const auto& h : imported_hashes_) hashes.append(h);
        root["hashes"] = hashes;
        Json::Value groups(Json::arrayValue);
        for (const auto& [_, g] : groups_) groups.append(writeStats(g));
        root["groups"] = groups;
        Json::Value times(Json::arrayValue);
        for (const auto& [_, g] : times_) times.append(writeStats(g));
        root["times"] = times;
    }
    std::error_code ec;
    const auto path = std::filesystem::path(config_.snapshot_file);
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            noteError("cannot write analytics snapshot: " + tmp);
            return false;
        }
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        out << Json::writeString(builder, root);
        if (!out.good()) {
            noteError("analytics snapshot write failed: " + tmp);
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        noteError("analytics snapshot rename failed: " + ec.message());
        return false;
    }
    std::error_code perm_ec;
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, perm_ec);
    return true;
}

bool ProviderAnalytics::loadSnapshot() {
    if (config_.snapshot_file.empty()) return false;
    std::ifstream in(config_.snapshot_file, std::ios::binary);
    if (!in) {
        noteError("cannot read analytics snapshot: " + config_.snapshot_file);
        return false;
    }
    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string errors;
    if (!Json::parseFromStream(builder, in, &root, &errors) || !root.isObject()) {
        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = "cannot parse analytics snapshot: " + errors;
        return false;
    }
    auto readSamples = [](const Json::Value& values, std::vector<double>& out) {
        for (const auto& v : values) {
            if (v.isNumeric()) out.push_back(v.asDouble());
        }
    };
    auto readStats = [&](const Json::Value& x) {
        Stats g;
        g.model = x.get("model", "").asString();
        g.provider = x.get("provider", "").asString();
        g.date = x.get("date", "").asString();
        g.hour = x.get("hour", -1).asInt();
        g.requests = x.get("requests", 0).asInt64();
        g.input_tokens = x.get("input_tokens", 0).asInt64();
        g.cached_tokens = x.get("cached_tokens", 0).asInt64();
        g.cache_write_tokens = x.get("cache_write_tokens", 0).asInt64();
        g.output_tokens = x.get("output_tokens", 0).asInt64();
        g.reasoning_tokens = x.get("reasoning_tokens", 0).asInt64();
        g.input_cost = x.get("input_cost", 0).asInt64();
        g.cache_cost = x.get("cache_cost", 0).asInt64();
        g.output_cost = x.get("output_cost", 0).asInt64();
        g.total_cost = x.get("total_cost", 0).asInt64();
        g.latency_sum_ms = x.get("latency_sum_ms", 0.0).asDouble();
        g.latency_max_ms = x.get("latency_max_ms", 0).asInt64();
        g.max_cached_tokens = x.get("max_cached_tokens", 0).asInt64();
        g.first_seen = x.get("first_seen", "").asString();
        g.last_seen = x.get("last_seen", "").asString();
        g.first_epoch = x.get("first_epoch", 0).asInt64();
        g.last_epoch = x.get("last_epoch", 0).asInt64();
        readSamples(x["input_prices"], g.input_prices);
        readSamples(x["cache_prices"], g.cache_prices);
        readSamples(x["output_prices"], g.output_prices);
        readSamples(x["latencies"], g.latencies_ms);
        finalize(g);
        return g;
    };
    std::lock_guard<std::mutex> lock(mu_);
    groups_.clear();
    times_.clear();
    imported_hashes_.clear();
    files_ = static_cast<std::size_t>(root.get("files", 0).asUInt64());
    rows_ = static_cast<std::size_t>(root.get("rows", 0).asUInt64());
    skipped_ = static_cast<std::size_t>(root.get("skipped", 0).asUInt64());
    for (const auto& h : root["hashes"]) {
        if (h.isString()) imported_hashes_.insert(h.asString());
    }
    for (const auto& x : root["groups"]) {
        auto g = readStats(x);
        groups_[groupKey(g.model, g.provider)] = std::move(g);
    }
    for (const auto& x : root["times"]) {
        auto g = readStats(x);
        times_[groupKey(g.model, g.provider) + "\x1f" + g.date + "\x1f" +
               std::to_string(g.hour)] = std::move(g);
    }
    last_error_.clear();
    return true;
}

Json::Value ProviderAnalytics::summaryJson() const {
    Json::Value v(Json::objectValue);
    v["enabled"] = config_.enabled;
    v["log_dir"] = config_.provider_log_dir;
    v["auto_import"] = config_.auto_import;
    std::lock_guard<std::mutex> lock(mu_);
    v["files"] = static_cast<Json::UInt64>(files_);
    v["rows"] = static_cast<Json::UInt64>(rows_);
    v["skipped"] = static_cast<Json::UInt64>(skipped_);
    v["error"] = last_error_;
    std::set<std::string> models;
    std::set<std::string> providers;
    std::int64_t requests = 0, input = 0, cached = 0, output = 0, reasoning = 0, cache_write = 0;
    NanoRub cost = 0;
    double latency = 0;
    for (const auto& [_, s] : groups_) {
        models.insert(s.model);
        providers.insert(s.provider);
        requests += s.requests;
        input += s.input_tokens;
        cached += s.cached_tokens;
        cache_write += s.cache_write_tokens;
        output += s.output_tokens;
        reasoning += s.reasoning_tokens;
        cost += s.total_cost;
        latency += s.latency_sum_ms;
    }
    v["models"] = static_cast<Json::UInt64>(models.size());
    v["providers"] = static_cast<Json::UInt64>(providers.size());
    v["requests"] = static_cast<Json::Int64>(requests);
    v["input_tokens"] = static_cast<Json::Int64>(input);
    v["cached_tokens"] = static_cast<Json::Int64>(cached);
    v["cache_write_tokens"] = static_cast<Json::Int64>(cache_write);
    v["output_tokens"] = static_cast<Json::Int64>(output);
    v["reasoning_tokens"] = static_cast<Json::Int64>(reasoning);
    v["cache_hit_ratio"] = ratioOf(cached, input);
    v["cost_nano"] = static_cast<Json::Int64>(cost);
    v["cost_rub"] = formatRub(cost);
    v["avg_latency_ms"] = requests > 0 ? latency / static_cast<double>(requests) : 0.0;
    return v;
}

bool ProviderAnalytics::byRequestsThenCost(const Stats& a, const Stats& b) {
    if (a.requests != b.requests) return a.requests > b.requests;
    return a.total_cost > b.total_cost;
}

Json::Value ProviderAnalytics::providersJson(std::size_t limit) const {
    std::vector<Stats> copy;
    {
        std::lock_guard<std::mutex> lock(mu_);
        copy.reserve(groups_.size());
        for (const auto& [_, s] : groups_) copy.push_back(s);
    }
    std::sort(copy.begin(), copy.end(), byRequestsThenCost);
    if (copy.size() > limit) copy.resize(limit);
    Json::Value arr(Json::arrayValue);
    for (const auto& s : copy) {
        Json::Value x(Json::objectValue);
        x["model"] = s.model;
        x["provider"] = s.provider;
        x["requests"] = static_cast<Json::Int64>(s.requests);
        x["input_tokens"] = static_cast<Json::Int64>(s.input_tokens);
        x["cached_tokens"] = static_cast<Json::Int64>(s.cached_tokens);
        x["cache_write_tokens"] = static_cast<Json::Int64>(s.cache_write_tokens);
        x["output_tokens"] = static_cast<Json::Int64>(s.output_tokens);
        x["reasoning_tokens"] = static_cast<Json::Int64>(s.reasoning_tokens);
        x["cache_hit_ratio"] = ratioOf(s.cached_tokens, s.input_tokens);
        x["max_cached_tokens"] = static_cast<Json::Int64>(s.max_cached_tokens);
        x["input_cost_nano"] = static_cast<Json::Int64>(s.input_cost);
        x["cache_cost_nano"] = static_cast<Json::Int64>(s.cache_cost);
        x["output_cost_nano"] = static_cast<Json::Int64>(s.output_cost);
        x["total_cost_nano"] = static_cast<Json::Int64>(s.total_cost);
        x["total_cost_rub"] = formatRub(s.total_cost);
        x["input_per_million_rub"] = formatRubPerMillion(s.input_per_token);
        x["cache_read_per_million_rub"] = formatRubPerMillion(s.cache_read_per_token);
        x["output_per_million_rub"] = formatRubPerMillion(s.output_per_token);
        x["avg_latency_ms"] = s.requests > 0 ? s.latency_sum_ms / static_cast<double>(s.requests) : 0.0;
        x["p50_latency_ms"] = s.p50_latency_ms;
        x["p90_latency_ms"] = s.p90_latency_ms;
        x["max_latency_ms"] = static_cast<Json::Int64>(s.latency_max_ms);
        x["tokens_per_second"] = s.tokens_per_second;
        x["output_tokens_per_second"] = s.output_tokens_per_second;
        x["reasoning_ratio"] = s.reasoning_ratio;
        x["avg_input_tokens"] = s.requests > 0
                                    ? static_cast<double>(s.input_tokens) / static_cast<double>(s.requests)
                                    : 0.0;
        x["avg_output_tokens"] = s.requests > 0
                                     ? static_cast<double>(s.output_tokens) / static_cast<double>(s.requests)
                                     : 0.0;
        x["avg_cost_rub"] = s.requests > 0
                                ? static_cast<double>(s.total_cost) /
                                      static_cast<double>(s.requests) /
                                      static_cast<double>(kNanoPerRub)
                                : 0.0;
        const auto total_tokens = s.input_tokens + s.output_tokens + s.reasoning_tokens;
        x["cost_per_million_tokens_rub"] =
            static_cast<double>(perMillionFromTotal(s.total_cost, total_tokens)) /
            static_cast<double>(kNanoPerRub);
        x["priced_samples"] = static_cast<Json::Int64>(
            std::max(s.input_prices.size(), s.output_prices.size()));
        x["first_seen"] = s.first_seen;
        x["last_seen"] = s.last_seen;
        arr.append(x);
    }
    return arr;
}

Json::Value ProviderAnalytics::modelsJson(std::size_t limit) const {
    std::map<std::string, Stats> models;
    std::map<std::string, std::set<std::string>> model_providers;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& [_, g] : groups_) {
            auto& m = models[g.model];
            m.model = g.model;
            m.requests += g.requests;
            m.input_tokens += g.input_tokens;
            m.cached_tokens += g.cached_tokens;
            m.cache_write_tokens += g.cache_write_tokens;
            m.output_tokens += g.output_tokens;
            m.reasoning_tokens += g.reasoning_tokens;
            m.input_cost += g.input_cost;
            m.cache_cost += g.cache_cost;
            m.output_cost += g.output_cost;
            m.total_cost += g.total_cost;
            m.latency_sum_ms += g.latency_sum_ms;
            m.latency_max_ms = std::max(m.latency_max_ms, g.latency_max_ms);
            m.max_cached_tokens = std::max(m.max_cached_tokens, g.max_cached_tokens);
            takeEarlier(m.first_epoch, m.first_seen, g.first_epoch, g.first_seen);
            takeLater(m.last_epoch, m.last_seen, g.last_epoch, g.last_seen);
            m.input_prices.insert(m.input_prices.end(), g.input_prices.begin(), g.input_prices.end());
            m.cache_prices.insert(m.cache_prices.end(), g.cache_prices.begin(), g.cache_prices.end());
            m.output_prices.insert(m.output_prices.end(), g.output_prices.begin(), g.output_prices.end());
            m.latencies_ms.insert(m.latencies_ms.end(), g.latencies_ms.begin(), g.latencies_ms.end());
            model_providers[g.model].insert(g.provider);
        }
    }
    std::vector<Stats> rows;
    for (auto& [_, m] : models) {
        finalize(m);
        rows.push_back(std::move(m));
    }
    std::sort(rows.begin(), rows.end(), byRequestsThenCost);
    if (rows.size() > limit) rows.resize(limit);
    Json::Value arr(Json::arrayValue);
    for (const auto& s : rows) {
        Json::Value x(Json::objectValue);
        x["model"] = s.model;
        x["providers"] = static_cast<Json::UInt64>(model_providers[s.model].size());
        x["requests"] = static_cast<Json::Int64>(s.requests);
        x["input_tokens"] = static_cast<Json::Int64>(s.input_tokens);
        x["cached_tokens"] = static_cast<Json::Int64>(s.cached_tokens);
        x["output_tokens"] = static_cast<Json::Int64>(s.output_tokens);
        x["reasoning_tokens"] = static_cast<Json::Int64>(s.reasoning_tokens);
        x["cache_hit_ratio"] = ratioOf(s.cached_tokens, s.input_tokens);
        x["input_per_million_rub"] = formatRubPerMillion(s.input_per_token);
        x["cache_read_per_million_rub"] = formatRubPerMillion(s.cache_read_per_token);
        x["output_per_million_rub"] = formatRubPerMillion(s.output_per_token);
        x["total_cost_nano"] = static_cast<Json::Int64>(s.total_cost);
        x["total_cost_rub"] = formatRub(s.total_cost);
        x["avg_cost_rub"] = s.requests > 0
                                ? static_cast<double>(s.total_cost) /
                                      static_cast<double>(s.requests) /
                                      static_cast<double>(kNanoPerRub)
                                : 0.0;
        const auto total_tokens = s.input_tokens + s.output_tokens + s.reasoning_tokens;
        x["cost_per_million_tokens_rub"] =
            static_cast<double>(perMillionFromTotal(s.total_cost, total_tokens)) /
            static_cast<double>(kNanoPerRub);
        x["avg_input_tokens"] = s.requests > 0
                                    ? static_cast<double>(s.input_tokens) / static_cast<double>(s.requests)
                                    : 0.0;
        x["avg_output_tokens"] = s.requests > 0
                                     ? static_cast<double>(s.output_tokens) / static_cast<double>(s.requests)
                                     : 0.0;
        x["avg_latency_ms"] = s.requests > 0
                                  ? s.latency_sum_ms / static_cast<double>(s.requests)
                                  : 0.0;
        x["p50_latency_ms"] = s.p50_latency_ms;
        x["p90_latency_ms"] = s.p90_latency_ms;
        x["tokens_per_second"] = s.tokens_per_second;
        x["output_tokens_per_second"] = s.output_tokens_per_second;
        x["reasoning_ratio"] = s.reasoning_ratio;
        x["max_cached_tokens"] = static_cast<Json::Int64>(s.max_cached_tokens);
        x["first_seen"] = s.first_seen;
        x["last_seen"] = s.last_seen;
        arr.append(x);
    }
    return arr;
}

Json::Value ProviderAnalytics::timesJson(std::size_t limit) const {
    std::vector<Stats> copy;
    {
        std::lock_guard<std::mutex> lock(mu_);
        copy.reserve(times_.size());
        for (const auto& [_, t] : times_) copy.push_back(t);
    }
    std::sort(copy.begin(), copy.end(), [](const auto& a, const auto& b) {
        if (a.date != b.date) return a.date > b.date;
        return a.hour > b.hour;
    });
    if (copy.size() > limit) copy.resize(limit);
    Json::Value arr(Json::arrayValue);
    for (const auto& t : copy) {
        Json::Value x(Json::objectValue);
        x["model"] = t.model;
        x["provider"] = t.provider;
        x["date"] = t.date;
        x["hour"] = t.hour;
        x["requests"] = static_cast<Json::Int64>(t.requests);
        x["input_tokens"] = static_cast<Json::Int64>(t.input_tokens);
        x["cached_tokens"] = static_cast<Json::Int64>(t.cached_tokens);
        x["output_tokens"] = static_cast<Json::Int64>(t.output_tokens);
        x["reasoning_tokens"] = static_cast<Json::Int64>(t.reasoning_tokens);
        x["cache_hit_ratio"] = ratioOf(t.cached_tokens, t.input_tokens);
        x["input_per_million_rub"] = formatRubPerMillion(t.input_per_token);
        x["cache_read_per_million_rub"] = formatRubPerMillion(t.cache_read_per_token);
        x["output_per_million_rub"] = formatRubPerMillion(t.output_per_token);
        x["total_cost_nano"] = static_cast<Json::Int64>(t.total_cost);
        x["total_cost_rub"] = formatRub(t.total_cost);
        x["avg_latency_ms"] = t.requests > 0 ? t.latency_sum_ms / static_cast<double>(t.requests) : 0.0;
        x["tokens_per_second"] = t.tokens_per_second;
        x["output_tokens_per_second"] = t.output_tokens_per_second;
        arr.append(x);
    }
    return arr;
}

} // namespace proxy
