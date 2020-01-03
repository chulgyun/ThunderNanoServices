#ifndef __PLUGIN_COMPOSITOR_H
#define __PLUGIN_COMPOSITOR_H

#include "Module.h"
#include <interfaces/IComposition.h>
#include <interfaces/json/JsonData_Compositor.h>


namespace WPEFramework {
namespace Plugin {
    class Compositor : public PluginHost::IPlugin, public PluginHost::IWeb, public PluginHost::JSONRPC {
    private:
        Compositor(const Compositor&) = delete;
        Compositor& operator=(const Compositor&) = delete;
        
    private:
        class Notification : public Exchange::IComposition::INotification {
        private:
            Notification(const Notification&) = delete;
            Notification& operator=(const Notification&) = delete;

        public:
            Notification(Compositor* parent)
                : _parent(*parent)
                , _client()
                , _service()
            {
            }
            virtual ~Notification()
            {
            }

            void Initialize(PluginHost::IShell* service, Exchange::IComposition* client)
            {
                ASSERT(_service == nullptr);
                ASSERT(service != nullptr);
                ASSERT(_client == nullptr);
                ASSERT(client != nullptr);

                _client = client;
                _client->AddRef();

                _service = service;
                _service->AddRef();
                _client->Register(this);
            }
            void Deinitialize()
            {

                ASSERT(_service != nullptr);
                ASSERT(_client != nullptr);

                if (_client != nullptr) {
                    _client->Unregister(this);
                    _client->Release();
                    _client = nullptr;
                }

                if (_service != nullptr) {
                    _service->Release();
                    _service = nullptr;
                }
            }

            //   IComposition INotification methods
            // -------------------------------------------------------------------------------------------------------
            virtual void Attached(const string& name, Exchange::IComposition::IClient* client) override
            {
                _parent.Attached(name, client);
            }
            virtual void Detached(const string& name) override
            {
                _parent.Detached(name);
            }

            BEGIN_INTERFACE_MAP(PluginSink)
            INTERFACE_ENTRY(Exchange::IComposition::INotification)
            END_INTERFACE_MAP

        private:
            Compositor& _parent;
            Exchange::IComposition* _client;
            PluginHost::IShell* _service;
        };

    public:
        class Config : public Core::JSON::Container {
        public:
            Config(const Config&);
            Config& operator=(const Config&);

        public:
            Config()
                : Core::JSON::Container()
                , System(_T("Controller"))
                , WorkDir()
            {
                Add(_T("system"), &System);
                Add(_T("workdir"), &WorkDir);
            }
            ~Config()
            {
            }

        public:
            Core::JSON::String System;
            Core::JSON::String WorkDir;
        };

    public:
        class Data : public Core::JSON::Container {
        private:
            Data(const Data&) = delete;
            Data& operator=(const Data&) = delete;

        public:
            Data()
                : Core::JSON::Container()
                , Clients()
            {
                Add(_T("clients"), &Clients);
                Add(_T("resolution"), &Resolution);
                Add(_T("x"), &X);
                Add(_T("y"), &Y);
                Add(_T("width"), &Width);
                Add(_T("height"), &Height);
            }

            virtual ~Data()
            {
            }

        public:
            Core::JSON::ArrayType<Core::JSON::String> Clients;
            Core::JSON::EnumType<Exchange::IComposition::ScreenResolution> Resolution;
            Core::JSON::DecUInt32 X;
            Core::JSON::DecUInt32 Y;
            Core::JSON::DecUInt32 Width;
            Core::JSON::DecUInt32 Height;
        };

    public:
        Compositor();
        virtual ~Compositor();

    public:
        BEGIN_INTERFACE_MAP(Compositor)
        INTERFACE_ENTRY(PluginHost::IPlugin)
        INTERFACE_ENTRY(PluginHost::IWeb)
        INTERFACE_ENTRY(PluginHost::IDispatcher)
        INTERFACE_AGGREGATE(Exchange::IComposition, _composition)
        END_INTERFACE_MAP

    public:
        //   IPlugin methods
        // -------------------------------------------------------------------------------------------------------
        virtual const string Initialize(PluginHost::IShell* service) override;
        virtual void Deinitialize(PluginHost::IShell* service) override;
        virtual string Information() const override;

        //	IWeb methods
        // -------------------------------------------------------------------------------------------------------
        virtual void Inbound(Web::Request& request) override;
        virtual Core::ProxyType<Web::Response> Process(const Web::Request& request) override;

    private:
        void Attached(const string& name, Exchange::IComposition::IClient* client);
        void Detached(const string& name);

        template <typename ClientOperation>
        uint32_t CallOnClientByCallsign(const string& callsign, ClientOperation&& operation) const;

        void Clients(Core::JSON::ArrayType<Core::JSON::String>& callsigns) const;
        uint32_t Kill(const string& callsign) const;
        uint32_t Opacity(const string& callsign, const uint32_t value) const;
        void Resolution(const Exchange::IComposition::ScreenResolution);
        Exchange::IComposition::ScreenResolution Resolution() const;
        uint32_t Visible(const string& callsign, const bool visible) const;
        uint32_t Geometry(const string& callsign, const Exchange::IComposition::Rectangle& rectangle);
        Exchange::IComposition::Rectangle Geometry(const string& callsign) const;
        uint32_t ToTop(const string& callsign);
        uint32_t PutBelow(const string& callsignRelativeTo, const string& callsignToReorder);
        void ZOrder(Core::JSON::ArrayType<Core::JSON::String>& callsigns) const;

    private:
        /* JSON-RPC */
        void RegisterAll();
        void UnregisterAll();
        uint32_t endpoint_putontop(const JsonData::Compositor::PutontopParamsInfo& params);
        uint32_t endpoint_putbelow(const JsonData::Compositor::PutbelowParamsData& params);
        uint32_t endpoint_kill(const JsonData::Compositor::PutontopParamsInfo& params);
        uint32_t get_resolution(Core::JSON::EnumType<JsonData::Compositor::ResolutionType>& response) const;
        uint32_t set_resolution(const Core::JSON::EnumType<JsonData::Compositor::ResolutionType>& param);
        uint32_t get_clients(Core::JSON::ArrayType<Core::JSON::String>& response) const;
        uint32_t get_zorder(Core::JSON::ArrayType<Core::JSON::String>& response) const;
        uint32_t get_geometry(const string& index, JsonData::Compositor::GeometryData& response) const;
        uint32_t set_geometry(const string& index, const JsonData::Compositor::GeometryData& param);
        uint32_t set_visiblity(const string& index, const Core::JSON::EnumType<JsonData::Compositor::VisiblityType>& param);
        uint32_t set_opacity(const string& index, const Core::JSON::DecUInt8& param);

    private:
        mutable Core::CriticalSection _adminLock;
        uint8_t _skipURL;
        Core::Sink<Notification> _notification;
        Exchange::IComposition* _composition;
        PluginHost::IShell* _service;
        uint32_t _connectionId;
        std::map<string, Exchange::IComposition::IClient*> _clients;
    };
}
}

#endif // __PLUGIN_COMPOSITOR_H
