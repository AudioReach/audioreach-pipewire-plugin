-- Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
-- SPDX-License-Identifier: BSD-3-Clause

log = Log.open_topic("device-detection")

----------------------------------------------------------------------
-- 1. Global state
----------------------------------------------------------------------

local default_metadata
local last_known_sink, last_known_source
local pending_metadata_write = { key = nil, name = nil }

local DEVICE_MAPPINGS = {
    connected = {
        ["Audio/Sink"] = {
            pal_sink_speaker_ll = "pal_sink_headset_ll",
            pal_sink_speaker_db = "pal_sink_headset_db",
            pal_sink_headset_ll = "pal_sink_headset_ll",
            pal_sink_headset_db = "pal_sink_headset_db",
        },
        ["Audio/Source"] = {
            pal_source_speaker_mic = "pal_source_headset_mic",
            pal_source_headset_mic = "pal_source_headset_mic",
        },
    },
    disconnected = {
        ["Audio/Sink"] = {
            pal_sink_headset_ll = "pal_sink_speaker_ll",
            pal_sink_headset_db = "pal_sink_speaker_db",
            pal_sink_speaker_ll = "pal_sink_speaker_ll",
            pal_sink_speaker_db = "pal_sink_speaker_db",
        },
        ["Audio/Source"] = {
            pal_source_headset_mic = "pal_source_speaker_mic",
            pal_source_speaker_mic = "pal_source_speaker_mic",
        },
    },
}

----------------------------------------------------------------------
-- 2. Helper: get current default sink/source name
----------------------------------------------------------------------

local function get_current_default(metadata, is_sink)
    local key       = is_sink and "default.audio.sink" or "default.audio.source"
    local type_name = is_sink and "sink" or "source"

    local obj = metadata and metadata:find(0, key)
    if not obj then
        log:warning("No current default " .. type_name .. " set")
        return nil
    end

    local name
    local ok, err = pcall(function()
        local parsed = Json.Raw(obj):parse()
        name = parsed and parsed.name
    end)

    if not ok or not name then
        local raw = tostring(obj)
        log:warning("Failed to parse metadata for " .. type_name ..
                    ": " .. tostring(err or "no name field") ..
                    ", raw=" .. raw)
        if raw:find("{") then
            name = raw:match('"name"%s*:%s*"([^"]+)"') or raw
        else
            name = raw
        end
    end

    log:warning("Current default " .. type_name .. ": " .. tostring(name))
    return name
end

----------------------------------------------------------------------
-- 3. Helper: set default sink/source (runtime + configured)
----------------------------------------------------------------------

local function set_default_device(metadata, is_sink, name)
    local type_name = is_sink and "sink" or "source"
    local runtime_key    = is_sink and "default.audio.sink" or "default.audio.source"
    local configured_key = is_sink and "default.configured.audio.sink"
                                  or "default.configured.audio.source"

    pending_metadata_write.key  = runtime_key
    pending_metadata_write.name = name

    if is_sink then last_known_sink = name else last_known_source = name end

    local ok, err = pcall(function()
        local v = '{"name":"' .. name .. '"}'
        metadata:set(0, runtime_key,    "Spa:String:JSON", v)
        metadata:set(0, configured_key, "Spa:String:JSON", v)
    end)

    if not ok then
        log:warning("Failed to set default " .. type_name .. ": " .. tostring(err))
        pending_metadata_write.key, pending_metadata_write.name = nil, nil
        return false
    end

    log:warning("Successfully set default " .. type_name .. " to: " .. name)

    GLib.timeout_add(GLib.PRIORITY_DEFAULT, 100, function()
        pending_metadata_write.key, pending_metadata_write.name = nil, nil
        return GLib.SOURCE_REMOVE
    end)

    return true
end

----------------------------------------------------------------------
-- 4. Metadata change hook: track external changes
----------------------------------------------------------------------

local function register_metadata_change_hook()
    log:warning("Registering metadata change hook")

    SimpleEventHook {
        name = "metadata-change-monitor",
        interests = {
            EventInterest {
                Constraint { "event.type", "=", "metadata-changed" },
                Constraint { "metadata.name", "=", "default" },
                Constraint { "event.subject.key", "c",
                    "default.audio.sink",
                    "default.audio.source",
                    "default.configured.audio.sink",
                    "default.configured.audio.source",
                },
            },
        },
        execute = function (event)
            if not default_metadata then return end

            local props       = event:get_properties() or {}
            local changed_key = props["event.subject.key"]
            log:warning("metadata-change-monitor: key changed = " .. tostring(changed_key))
            if not changed_key then return end

            if pending_metadata_write.key ~= nil and changed_key == pending_metadata_write.key then
                local is_sink_key = (changed_key == "default.audio.sink") or
                                    (changed_key == "default.configured.audio.sink")
                local current_name = get_current_default(default_metadata, is_sink_key)
                if current_name == pending_metadata_write.name then
                    log:warning("Change matches our own metadata:set (key=" ..
                                changed_key .. ", name=" .. tostring(current_name) .. "); ignoring")
                    return
                else
                    log:warning("Pending write exists but value differs (ours=" ..
                                tostring(pending_metadata_write.name) ..
                                ", current=" .. tostring(current_name) ..
                                "); treating as external change")
                end
            end

            if changed_key == "default.audio.sink" or
               changed_key == "default.configured.audio.sink" then

                local current_sink = get_current_default(default_metadata, true)
                if current_sink and current_sink ~= "Spa:String:JSON" then
                    last_known_sink = current_sink
                    log:warning("Updated last known sink to: " .. last_known_sink)
                end

            elseif changed_key == "default.audio.source" or
                   changed_key == "default.configured.audio.source" then

                local current_source = get_current_default(default_metadata, false)
                if current_source and current_source ~= "Spa:String:JSON" then
                    last_known_source = current_source
                    log:warning("Updated last known source to: " .. last_known_source)
                end
            end
        end,
    }:register()

    log:warning("Metadata change monitor registered successfully")
end

----------------------------------------------------------------------
-- 5. Jack hook: reacts to jack.connected changes
----------------------------------------------------------------------

local function register_jack_hook()
    log:warning("Registering jack param-changes hook (metadata is ready)")

    SimpleEventHook {
        name = "param-changes-monitor",
        interests = {
            EventInterest {
                Constraint { "event.type", "=", "node-params-changed" },
                Constraint { "event.subject.param-id", "=", "Props" },
            },
        },
        execute = function (event)
            local node = event:get_subject()
            if not node or not node.properties then
                log:warning("No node or node.properties")
                return
            end

            if not default_metadata then
                log:warning("Metadata not ready, skipping node")
                return
            end

            local props          = node.properties
            local name           = props["node.name"] or "<unnamed>"
            local jack_connected = props["jack.connected"]
            local media_class    = props["media.class"] or ""

            if jack_connected == nil then
                log:warning("jack.connected is nil, skipping " .. name)
                return
            end

            local is_connected = (jack_connected == true or jack_connected == "true")
            local jack_state   = is_connected and "connected" or "disconnected"
            local is_sink      = media_class:find("Audio/Sink")   ~= nil
            local is_source    = media_class:find("Audio/Source") ~= nil

            if not (is_sink or is_source) then
                log:warning("Node " .. name .. " is neither sink nor source: " .. media_class)
                return
            end

            local current_default = get_current_default(default_metadata, is_sink)
            log:warning("Current runtime default for " ..
                        (is_sink and "sink" or "source") ..
                        ": " .. tostring(current_default))

            if is_sink then
                local cur_ll  = current_default and current_default:find("_ll") ~= nil
                local cur_db  = current_default and current_default:find("_db") ~= nil
                local node_ll = name:find("_ll") ~= nil
                local node_db = name:find("_db") ~= nil

                if (cur_ll and not node_ll) or (cur_db and not node_db) then
                    log:warning("Skipping " .. name .. " (flavor mismatch)")
                    return
                end
            end

            local class_key        = is_sink and "Audio/Sink" or "Audio/Source"
            local mappings_by_state = DEVICE_MAPPINGS[jack_state]
            local class_mappings    = mappings_by_state and mappings_by_state[class_key]

            local base_default = is_sink and last_known_sink or last_known_source
            log:warning("Base default for " .. class_key .. ": " .. tostring(base_default))

            -- Determine mapping for the *current* default
            local mapped = nil
            if class_mappings and base_default then
                mapped = class_mappings[base_default]
            end

            if base_default and not mapped then
                -- Case 1: we *do* have a current default, but it is NOT in the mapping.
                -- This is your “unmapped” case (e.g. pal_sink_speaker_compress).
                -- Do NOT change the default on headset plug/unplug.
                log:warning(
                    "No mapping for base_default=" .. tostring(base_default) ..
                    " in state=" .. jack_state .. "; keeping existing default"
                )

                -- You can still activate the node if you want:
                if type(node.activate) == "function" then
                    log:warning("Activating node " .. name)
                    node:activate(1)
                end
                return
            end

            -- Case 2: Either we have a valid mapping, or there is no known base_default.
            -- - If mapping exists: use it
            -- - If no base_default: fall back to node.name (first-time / broken metadata)
            local target_name = mapped or name

            if target_name == current_default then
                log:warning(class_key .. " " .. target_name .. " is already default; skipping")
            else
                log:warning("Setting default " .. class_key .. " to: " ..
                            target_name .. " (from " .. (current_default or "none") .. ")")
                set_default_device(default_metadata, is_sink, target_name)
            end

            if type(node.activate) == "function" then
                log:warning("Activating node " .. name)
                node:activate(1)
            end
        end,
    }:register()

    log:warning("JACK connection monitor registered successfully")
end

----------------------------------------------------------------------
-- 6. ObjectManager to wait for 'default' metadata, then register hooks
----------------------------------------------------------------------

local metadata_om = ObjectManager {
    Interest {
        type = "metadata",
        Constraint { "metadata.name", "=", "default" },
    }
}

metadata_om:connect("objects-changed", function (om)
    if default_metadata then return end

    log:warning("metadata_om objects-changed; looking for default metadata")

    local ok, obj = pcall(function() return om:lookup() end)
    if not ok then
        log:warning("metadata_om lookup() failed: " .. tostring(obj))
        return
    end
    if not obj then
        log:warning("metadata_om lookup() returned nil")
        return
    end

    default_metadata = obj
    log:warning("default metadata object found: " .. tostring(obj))

    last_known_sink   = get_current_default(default_metadata, true)
    last_known_source = get_current_default(default_metadata, false)
    log:warning("Initialized last known sink: " .. tostring(last_known_sink))
    log:warning("Initialized last known source: " .. tostring(last_known_source))

    register_jack_hook()
    register_metadata_change_hook()
end)

metadata_om:activate()
log:warning("metadata ObjectManager activated, waiting for default metadata...")