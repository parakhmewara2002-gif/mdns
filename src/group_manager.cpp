// ============================================================
//  group_manager.cpp
// ============================================================
#include "group_manager.h"
#include <LittleFS.h>
#include <algorithm>

GroupManager groupMgr;

GroupManager::GroupManager() : _nextId(1) {}

void GroupManager::begin() {
    if (!loadFromFile()) {
        _ensureDefault();
        saveToFile();
    }
}

void GroupManager::_ensureDefault() {
    if (_groups.empty()) {
        _groups.emplace_back(_nextId++, DEFAULT_GROUP_NAME, "📺", 0);
    }
}

uint32_t GroupManager::add(const String& name, const String& icon) {
    if (_groups.size() >= MAX_GROUPS) return 0;
    String n = name; n.trim();
    if (n.isEmpty() || n.length() > GROUP_NAME_MAX) return 0;
    uint8_t ord = (uint8_t)_groups.size();
    uint32_t id = _nextId++;
    _groups.emplace_back(id, n, icon, ord);
    saveToFile();
    return id;
}

bool GroupManager::update(uint32_t id, const String& name, const String& icon) {
    for (auto& g : _groups) {
        if (g.id == id) {
            String n = name; n.trim();
            if (n.isEmpty() || n.length() > GROUP_NAME_MAX) return false;
            g.name = n;
            g.icon = icon;
            saveToFile();
            return true;
        }
    }
    return false;
}

bool GroupManager::remove(uint32_t id) {
    // Cannot remove if it's the only group
    if (_groups.size() <= 1) return false;
    for (auto it = _groups.begin(); it != _groups.end(); ++it) {
        if (it->id == id) {
            _groups.erase(it);
            saveToFile();
            return true;
        }
    }
    return false;
}

bool GroupManager::reorder(uint32_t id, uint8_t newOrder) {
    for (auto& g : _groups) {
        if (g.id == id) {
            g.order = newOrder;
            // Sort by order
            std::sort(_groups.begin(), _groups.end(),
                [](const IRGroup& a, const IRGroup& b){ return a.order < b.order; });
            saveToFile();
            return true;
        }
    }
    return false;
}

bool GroupManager::reorderAll(const std::vector<uint32_t>& orderedIds) {
    if (orderedIds.empty()) return false;
    // Assign order values based on position in orderedIds array
    for (size_t pos = 0; pos < orderedIds.size(); ++pos) {
        uint32_t gid = orderedIds[pos];
        for (auto& g : _groups) {
            if (g.id == gid) { g.order = (uint8_t)pos; break; }
        }
    }
    std::sort(_groups.begin(), _groups.end(),
        [](const IRGroup& a, const IRGroup& b){ return a.order < b.order; });
    saveToFile();
    return true;
}

const IRGroup* GroupManager::findById(uint32_t id) const {
    for (const auto& g : _groups)
        if (g.id == id) return &g;
    return nullptr;
}

String GroupManager::toJson() const {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& g : _groups) {
        JsonObject o = arr.add<JsonObject>();
        g.toJson(o);
    }
    String out; serializeJson(doc, out);
    return out;
}

bool GroupManager::loadFromFile() {
    if (!LittleFS.exists(GROUPS_FILE)) return false;
    File f = LittleFS.open(GROUPS_FILE, "r");
    if (!f) return false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err != DeserializationError::Ok || !doc.is<JsonArrayConst>()) return false;

    _groups.clear();
    _nextId = 1;
    for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
        IRGroup g;
        if (g.fromJson(o)) {
            _groups.push_back(g);
            if (g.id >= _nextId) _nextId = g.id + 1;
        }
    }
    if (_groups.empty()) { _ensureDefault(); return false; }
    // Sort by order
    std::sort(_groups.begin(), _groups.end(),
        [](const IRGroup& a, const IRGroup& b){ return a.order < b.order; });
    Serial.printf(DEBUG_TAG " Groups loaded: %u\n", (unsigned)_groups.size());
    return true;
}

bool GroupManager::saveToFile() {
    File f = LittleFS.open(GROUPS_FILE, "w");
    if (!f) { Serial.println(DEBUG_TAG " ERROR: Cannot write groups."); return false; }
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& g : _groups) {
        JsonObject o = arr.add<JsonObject>();
        g.toJson(o);
    }
    size_t w = serializeJson(doc, f); f.close();
    return w > 0;
}
