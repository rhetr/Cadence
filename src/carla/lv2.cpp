/*
 * JACK Backend code for Carla
 * Copyright (C) 2011-2012 Filipe Coelho <falktx@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the COPYING file
 */

#include "carla_plugin.h"

#include "lv2/lv2.h"
#include "lv2/event.h"
#include "lv2/event-helpers.h"
#include "lv2/uri-map.h"
#include "lv2/urid.h"
#include "lv2/ui.h"

#include "lv2_rdf.h"

// static max values
const unsigned int MAX_EVENT_BUFFER = 0x7FFF; // 32767

// feature ids
const uint32_t lv2_feature_id_uri_map         = 0;
const uint32_t lv2_feature_id_urid_map        = 1;
const uint32_t lv2_feature_id_urid_unmap      = 2;
const uint32_t lv2_feature_id_event           = 3;
//const uint32_t lv2_feature_id_rtmempool       = 4;
//const uint32_t lv2_feature_id_data_access     = 5;
//const uint32_t lv2_feature_id_instance_access = 6;
//const uint32_t lv2_feature_id_ui_resize       = 7;
//const uint32_t lv2_feature_id_ui_parent       = 8;
//const uint32_t lv2_feature_id_external_ui     = 9;
//const uint32_t lv2_feature_id_external_ui_old = 10;
const uint32_t lv2_feature_count              = 4; //11;

// extra plugin hints
const unsigned int PLUGIN_HAS_EXTENSION_STATE    = 0x1000;
const unsigned int PLUGIN_HAS_EXTENSION_DYNPARAM = 0x2000;

// uri[d] map ids
const uint32_t CARLA_URI_MAP_ID_ATOM_STRING   = 1;
const uint32_t CARLA_URI_MAP_ID_EVENT_MIDI    = 2;
const uint32_t CARLA_URI_MAP_ID_EVENT_TIME    = 3;
const uint32_t CARLA_URI_MAP_ID_COUNT         = 4;

struct CarlaLv2Event {
    uint16_t type;
    jack_port_t* port;
    LV2_Event_Buffer* buffer;
};

struct CarlaLv2EventData {
    uint32_t count;
    CarlaLv2Event* events;
};

class Lv2Plugin : public CarlaPlugin
{
public:
    Lv2Plugin() :
        CarlaPlugin()
    {
        qDebug("Lv2Plugin::Lv2Plugin()");
        m_type = PLUGIN_LV2;

        evin.count  = 0;
        evin.events = nullptr;

        evout.count  = 0;
        evout.events = nullptr;

        handle = nullptr;
        descriptor = nullptr;
        rdf_descriptor = nullptr;

        ui.lib = nullptr;
        ui.handle = nullptr;
        ui.descriptor = nullptr;
        ui.rdf_descriptor = nullptr;

        for (uint32_t i=0; i < lv2_feature_count+1; i++)
            features[i] = nullptr;

        // Fill pre-set URI keys
        for (uint32_t i=0; i < CARLA_URI_MAP_ID_COUNT; i++)
            custom_uri_ids.append(nullptr);
    }

    virtual ~Lv2Plugin()
    {
        qDebug("Lv2Plugin::~Lv2Plugin()");

        for (int i=0; i < custom_uri_ids.count(); i++)
        {
            if (custom_uri_ids[i])
                free((void*)custom_uri_ids[i]);
        }

        custom_uri_ids.clear();

        if (handle && descriptor->deactivate && m_active_before)
            descriptor->deactivate(handle);

        if (handle && descriptor->cleanup)
            descriptor->cleanup(handle);

        if (features[lv2_feature_id_uri_map] && features[lv2_feature_id_uri_map]->data)
            delete (LV2_URI_Map_Feature*)features[lv2_feature_id_uri_map]->data;

        if (features[lv2_feature_id_urid_map] && features[lv2_feature_id_urid_map]->data)
            delete (LV2_URID_Map*)features[lv2_feature_id_urid_map]->data;

        if (features[lv2_feature_id_urid_unmap] && features[lv2_feature_id_urid_unmap]->data)
            delete (LV2_URID_Unmap*)features[lv2_feature_id_urid_unmap]->data;

        if (features[lv2_feature_id_event] && features[lv2_feature_id_event]->data)
            delete (LV2_Event_Feature*)features[lv2_feature_id_event]->data;

        for (uint32_t j=0; j<lv2_feature_count && features[j]; j++)
            delete features[j];

        if (rdf_descriptor)
            lv2_rdf_free(rdf_descriptor);

        lv2_remove_from_jack();

        lv2_delete_buffers();
    }

    void lv2_remove_from_jack()
    {
        qDebug("Lv2Plugin::lv2_remove_from_jack()");

        uint32_t i;

        for (i=0; i < evin.count; i++)
            jack_port_unregister(jack_client, evin.events[i].port);

        for (i=0; i < evout.count; i++)
            jack_port_unregister(jack_client, evout.events[i].port);

        qDebug("Lv2Plugin::lv2_remove_from_jack() - end");
    }

    void lv2_delete_buffers()
    {
        qDebug("Lv2Plugin::lv2_delete_buffers()");

        if (evin.count > 0)
        {
            for (uint32_t i=0; i < evin.count; i++)
                free(evin.events[i].buffer);

            delete[] evin.events;
        }

        if (evout.count > 0)
        {
            for (uint32_t i=0; i < evout.count; i++)
                free(evout.events[i].buffer);

            delete[] evout.events;
        }

        evin.count  = 0;
        evin.events = nullptr;

        evout.count  = 0;
        evout.events = nullptr;

        qDebug("Lv2Plugin::lv2_delete_buffers() - end");
    }

    uint32_t get_custom_uri_id(const char* uri)
    {
        qDebug("Lv2Plugin::get_custom_uri_id(%s)", uri);

        for (int i=0; i < custom_uri_ids.count(); i++)
        {
            if (custom_uri_ids[i] && strcmp(custom_uri_ids[i], uri) == 0)
                return i;
        }

        custom_uri_ids.append(strdup(uri));
        return custom_uri_ids.count()-1;
    }

    const char* get_custom_uri_string(int uri_id)
    {
        qDebug("Lv2Plugin::get_custom_uri_string(%i)", uri_id);

        if (uri_id < custom_uri_ids.count())
            return custom_uri_ids.at(uri_id);
        else
            return nullptr;
    }

    virtual PluginCategory category()
    {
        LV2_Property Category = rdf_descriptor->Type;

        // Specific Types
        if (Category & LV2_CLASS_REVERB)
            return PLUGIN_CATEGORY_DELAY;

        // Pre-set LV2 Types
        else if (LV2_IS_GENERATOR(Category))
            return PLUGIN_CATEGORY_SYNTH;
        else if (LV2_IS_UTILITY(Category))
            return PLUGIN_CATEGORY_UTILITY;
        else if (LV2_IS_SIMULATOR(Category))
            return PLUGIN_CATEGORY_OUTRO;
        else if (LV2_IS_DELAY(Category))
            return PLUGIN_CATEGORY_DELAY;
        else if (LV2_IS_MODULATOR(Category))
            return PLUGIN_CATEGORY_MODULATOR;
        else if (LV2_IS_FILTER(Category))
            return PLUGIN_CATEGORY_FILTER;
        else if (LV2_IS_EQUALISER(Category))
            return PLUGIN_CATEGORY_EQ;
        else if (LV2_IS_SPECTRAL(Category))
            return PLUGIN_CATEGORY_UTILITY;
        else if (LV2_IS_DISTORTION(Category))
            return PLUGIN_CATEGORY_OUTRO;
        else if (LV2_IS_DYNAMICS(Category))
            return PLUGIN_CATEGORY_DYNAMICS;
        else
            return PLUGIN_CATEGORY_NONE;
    }

    virtual long unique_id()
    {
        return rdf_descriptor->UniqueID;
    }

    virtual void get_label(char* buf_str)
    {
        strncpy(buf_str, rdf_descriptor->URI, STR_MAX);
    }

    virtual void get_maker(char* buf_str)
    {
        strncpy(buf_str, rdf_descriptor->Author, STR_MAX);
    }

    virtual void get_copyright(char* buf_str)
    {
        strncpy(buf_str, rdf_descriptor->License, STR_MAX);
    }

    virtual void get_real_name(char* buf_str)
    {
        strncpy(buf_str, rdf_descriptor->Name, STR_MAX);
    }

    virtual void get_parameter_name(uint32_t index, char* buf_str)
    {
        strncpy(buf_str, rdf_descriptor->Ports[index].Name, STR_MAX);
    }

    virtual void get_parameter_symbol(uint32_t index, char* buf_str)
    {
        strncpy(buf_str, rdf_descriptor->Ports[index].Symbol, STR_MAX);
    }

    bool init(const char* filename, const char* URI, void* extra_stuff)
    {
        LV2_RDF_Descriptor* rdf_descriptor_ = (LV2_RDF_Descriptor*)extra_stuff;

        qDebug("INIT %s", URI);

        if (rdf_descriptor_)
        {
            qDebug("INIT 002");
            if (lib_open(rdf_descriptor_->Binary))
            {
                qDebug("INIT 003");
                LV2_Descriptor_Function descfn = (LV2_Descriptor_Function)lib_symbol("lv2_descriptor");

                if (descfn)
                {
                    qDebug("INIT 004");
                    uint32_t i = 0;
                    while ((descriptor = descfn(i++)))
                    {
                        qDebug("%s | %s", descriptor->URI, URI);
                        if (strcmp(descriptor->URI, URI) == 0)
                            break;
                    }
                    qDebug("INIT 005");

                    if (descriptor)
                    {
                        // TODO - can continue

                        handle = descriptor->instantiate(descriptor, get_sample_rate(), rdf_descriptor_->Bundle, features);

                        if (handle)
                        {
                            m_filename = strdup(filename);
                            m_name = get_unique_name(rdf_descriptor_->Name);

                            rdf_descriptor = lv2_rdf_dup(rdf_descriptor_);

                            if (carla_jack_register_plugin(this, &jack_client))
                                return true;
                            else
                                set_last_error("Failed to register plugin in JACK");
                        }
                        else
                            set_last_error("Plugin failed to initialize");
                    }
                    else
                        set_last_error("Could not find the requested plugin URI in the plugin library");
                }
                else
                    set_last_error("Could not find the LV2 Descriptor in the plugin library");
            }
            else
                set_last_error(lib_error());
        }
        else
            set_last_error("Failed to find the requested plugin in the LV2 Bundle");

        return false;
    }

private:
    CarlaLv2EventData evin;
    CarlaLv2EventData evout;

    LV2_Handle handle;
    const LV2_Descriptor* descriptor;
    const LV2_RDF_Descriptor* rdf_descriptor;
    LV2_Feature* features[lv2_feature_count+1];

    struct {
        void* lib;
        LV2UI_Handle handle;
        LV2UI_Widget widget;
        const LV2UI_Descriptor* descriptor;
        const LV2_RDF_UI* rdf_descriptor;
    } ui;

    QList<const char*> custom_uri_ids;
};

short add_plugin_lv2(const char* filename, const char* label, void* extra_stuff)
{
    qDebug("add_plugin_lv2(%s, %s, %p)", filename, label, extra_stuff);

    short id = get_new_plugin_id();

    if (id >= 0)
    {
        Lv2Plugin* plugin = new Lv2Plugin;

        if (plugin->init(filename, label, extra_stuff))
        {
            plugin->reload();
            plugin->set_id(id);

            unique_names[id] = plugin->name();
            CarlaPlugins[id] = plugin;

            //osc_new_plugin(plugin);
        }
        else
        {
            delete plugin;
            id = -1;
        }
    }
    else
        set_last_error("Maximum number of plugins reached");

    return id;
}