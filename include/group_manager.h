#pragma once
// ============================================================
//  group_manager.h  -  Named button groups / remote presets
//
//  Supports up to MAX_GROUPS groups.
//  Each group has: id, name, icon, order index.
//  Buttons reference a group by groupId.
//  Groups are persisted to /groups.json on LittleFS.
// ============================================================
#include <Arduino.h>
#include <vector>
#include <ArduinoJson.h>
#include "config.h"

struct IRGroup {
    uint32_t id;
    String   name;
    String   icon;     // emoji or short string for Web GUI tab
    uint8_t  order;    // display order (lower = first)

    IRGroup() : id(0), order(0) {}
    IRGroup(uint32_t i, const String& n, const String& ico = "📺", uint8_t o = 0)
        : id(i), name(n), icon(ico), order(o) {}

    void toJson(JsonObject obj) const {
        obj["id"]    = id;
        obj["name"]  = name;
        obj["icon"]  = icon;
        obj["order"] = order;
    }
    bool fromJson(JsonObjectConst obj) {
        id    = obj["id"]    | (uint32_t)0;
        name  = obj["name"]  | (const char*)"";
        icon  = obj["icon"]  | (const char*)"📺";
        order = obj["order"] | (uint8_t)0;
        name.trim();
        return name.length() > 0;
    }
};

class GroupManager {
public:
    GroupManager();
    void begin();

    // CRUD
    uint32_t    add(const String& name, const String& icon = "📺");
    bool        update(uint32_t id, const String& name, const String& icon);
    bool        remove(uint32_t id);
    bool        reorder(uint32_t id, uint8_t newOrder);
    bool        reorderAll(const std::vector<uint32_t>& orderedIds);
    const IRGroup* findById(uint32_t id) const;
    size_t      size() const { return _groups.size(); }

    // Serialisation
    String      toJson() const;   // compact JSON array for API
    bool        loadFromFile();
    bool        saveToFile();

    // Access all groups
    const std::vector<IRGroup>& groups() const { return _groups; }

private:
    std::vector<IRGroup> _groups;
    uint32_t             _nextId;

    void _ensureDefault();
};

extern GroupManager groupMgr;
