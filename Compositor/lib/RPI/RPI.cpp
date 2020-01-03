#include "Module.h"

#include <interfaces/IComposition.h>

MODULE_NAME_DECLARATION(BUILD_REFERENCE)

namespace WPEFramework {
namespace Plugin {

    class CompositorImplementation : public Exchange::IComposition {
    private:
        CompositorImplementation(const CompositorImplementation&) = delete;
        CompositorImplementation& operator=(const CompositorImplementation&) = delete;

        class ExternalAccess : public RPC::Communicator {
        private:
            ExternalAccess() = delete;
            ExternalAccess(const ExternalAccess&) = delete;
            ExternalAccess& operator=(const ExternalAccess&) = delete;

        public:
            ExternalAccess(
                CompositorImplementation& parent, 
                const Core::NodeId& source, 
                const string& proxyStubPath, 
                const Core::ProxyType<RPC::InvokeServer>& handler)
                : RPC::Communicator(source,  proxyStubPath.empty() == false ? Core::Directory::Normalize(proxyStubPath) : proxyStubPath, Core::ProxyType<Core::IIPCServer>(handler))
                , _parent(parent)
            {
                uint32_t result = RPC::Communicator::Open(RPC::CommunicationTimeOut);

                handler->Announcements(Announcement());

                if (result != Core::ERROR_NONE) {
                    TRACE(Trace::Error, (_T("Could not open RPI Compositor RPCLink server. Error: %s"), Core::NumberType<uint32_t>(result).Text()));
                } else {
                    // We need to pass the communication channel NodeId via an environment variable, for process,
                    // not being started by the rpcprocess...
                    Core::SystemInfo::SetEnvironment(_T("COMPOSITOR"), RPC::Communicator::Connector(), true);
                }
            }

            virtual ~ExternalAccess() override = default;

        private:
            void Offer(Core::IUnknown* element, const uint32_t interfaceID) override
            {
                Exchange::IComposition::IClient* result = element->QueryInterface<Exchange::IComposition::IClient>();

                if (result != nullptr) {
                    _parent.NewClientOffered(result);
                    result->Release();
                }
            }

            void Revoke(const Core::IUnknown* element, const uint32_t interfaceID) override
            {
                _parent.ClientRevoked(element);
            }

        private:
            CompositorImplementation& _parent;
        };

    public:
        CompositorImplementation()
            : _adminLock()
            , _service(nullptr)
            , _engine()
            , _externalAccess(nullptr)
            , _observers()
            , _clients()
        {
        }

        ~CompositorImplementation()
        {
            if (_externalAccess != nullptr) {
                delete _externalAccess;
                _engine.Release();
            }
        }

        BEGIN_INTERFACE_MAP(CompositorImplementation)
        INTERFACE_ENTRY(Exchange::IComposition)
        END_INTERFACE_MAP

    private:
        class Config : public Core::JSON::Container {
        private:
            Config(const Config&) = delete;
            Config& operator=(const Config&) = delete;

        public:
            Config()
                : Core::JSON::Container()
                , Connector(_T("/tmp/compositor"))
            {
                Add(_T("connector"), &Connector);
            }

            ~Config()
            {
            }

        public:
            Core::JSON::String Connector;
        };

        struct ClientData {
            ClientData()
                : currentRectangle()
                , clientInterface(nullptr)
            {
            }
            ClientData(Exchange::IComposition::IClient* client, Exchange::IComposition::ScreenResolution resolution)
                : currentRectangle()
                , clientInterface(client)
            {
                clientInterface->AddRef();
                currentRectangle.x = 0;
                currentRectangle.y = 0;
                currentRectangle.width = Exchange::IComposition::WidthFromResolution(resolution);
                currentRectangle.height = Exchange::IComposition::HeightFromResolution(resolution);
            }
            ~ClientData() {
                if (clientInterface != nullptr) {
                    clientInterface->Release();
                }
            }

            Exchange::IComposition::Rectangle currentRectangle;
            Exchange::IComposition::IClient* clientInterface;
        };

    public:
        uint32_t Configure(PluginHost::IShell* service) override
        {
            uint32_t result = Core::ERROR_NONE;
            _service = service;

            string configuration(service->ConfigLine());
            Config config;
            config.FromString(service->ConfigLine());

            _engine = Core::ProxyType<RPC::InvokeServer>::Create(&Core::WorkerPool::Instance());
            _externalAccess = new ExternalAccess(*this, Core::NodeId(config.Connector.Value().c_str()), service->ProxyStubPath(), _engine);

            if (_externalAccess->IsListening() == true) {
                PlatformReady();
                
            } else {
                delete _externalAccess;
                _externalAccess = nullptr;
                _engine.Release();
                TRACE(Trace::Error, (_T("Could not report PlatformReady as there was a problem starting the Compositor RPC %s"), _T("server")));
                result = Core::ERROR_OPENING_FAILED;
            }
            return result;
        }

        void Register(Exchange::IComposition::INotification* notification) override
        {
            _adminLock.Lock();
            ASSERT(std::find(_observers.begin(),
                       _observers.end(), notification)
                == _observers.end());
            notification->AddRef();
            _observers.push_back(notification);
            auto index(_clients.begin());
            while (index != _clients.end()) {
                notification->Attached(index->first, index->second.clientInterface);
                index++;
            }
            _adminLock.Unlock();
        }

        void Unregister(Exchange::IComposition::INotification* notification) override
        {
            _adminLock.Lock();
            std::list<Exchange::IComposition::INotification*>::iterator index(
                std::find(_observers.begin(), _observers.end(), notification));
            ASSERT(index != _observers.end());
            if (index != _observers.end()) {
                _observers.erase(index);
                notification->Release();
            }
            _adminLock.Unlock();
        }

        Exchange::IComposition::IClient* Client(const uint8_t id) override
        {
            Exchange::IComposition::IClient* result = nullptr;
            _adminLock.Lock();
            auto index(_clients.begin());
            std::advance(index, id);
            if (index != _clients.end()) {
                result = (index->second.clientInterface);
                ASSERT(result != nullptr);
                result->AddRef();
            }
            _adminLock.Unlock();
            return (result);
        }

        Exchange::IComposition::IClient* Client(const string& name) override
        {
            return FindClient(name);
        }

    private:
        template <typename ClientOperation>
        uint32_t CallOnClientByCallsign(const string& callsign, ClientOperation&& operation)
        {
            uint32_t error = Core::ERROR_NONE;
            Exchange::IComposition::IClient* client = FindClient(callsign);
            if (client != nullptr) {
                std::forward<ClientOperation>(operation)(*client);
                client->Release();
            } else {
                error = Core::ERROR_FIRST_RESOURCE_NOT_FOUND;
            }
            return error;
        }

    public:
        uint32_t Geometry(const string& callsign, const Exchange::IComposition::Rectangle& rectangle) override
        {
            uint32_t result = CallOnClientByCallsign(callsign, [&](Exchange::IComposition::IClient& client) { client.ChangedGeometry(rectangle); });
            if (result == Core::ERROR_NONE) {
                result = SetClientRectangle(callsign, rectangle);
            }
            return result;
        }

        Exchange::IComposition::Rectangle Geometry(const string& callsign) const override
        {
            return FindClientRectangle(callsign);
        }

        uint32_t ToTop(const string& callsign) override
        {
            // todo correct implementation
            return CallOnClientByCallsign(callsign, [&](Exchange::IComposition::IClient& client) { client.ChangedZOrder(_clients.size()); });
        }

        uint32_t PutBelow(const string& callsignRelativeTo, const string& callsignToReorder) override
        {
            // todo correct implementation
            return CallOnClientByCallsign(callsignRelativeTo, [&](Exchange::IComposition::IClient& client) { client.ChangedZOrder(~0); });
        }

        RPC::IStringIterator* ClientsInZorder() const override
        {
            // todo correct implementation
            using CliensCallsignArray = std::vector<string>;
            _adminLock.Lock();

            CliensCallsignArray clients;
            clients.reserve(_clients.size());

            for (auto const& client : _clients) {
                clients.push_back(client.first); // todo for now RPC call inside lock, later on we need some map anyway
            }
            _adminLock.Unlock();
            return (Core::Service<RPC::StringIterator>::Create<RPC::IStringIterator>(clients));
        }

        void Resolution(const Exchange::IComposition::ScreenResolution format) override
        {
            TRACE(Trace::Information, (_T("Could not set screenresolution to %s. Not supported for Rapberry Pi compositor"), Core::EnumerateType<Exchange::IComposition::ScreenResolution>(format).Data()));
        }

        Exchange::IComposition::ScreenResolution Resolution() const override
        {
            return Exchange::IComposition::ScreenResolution::ScreenResolution_720p;
        }

    private:
        using ClientDataContainer = std::map<string, ClientData>;
        using ConstClientDataIterator = ClientDataContainer::const_iterator;

        ConstClientDataIterator GetClientIterator(const string& callsign) const
        {
            return _clients.find(callsign);
        }

        void NewClientOffered(Exchange::IComposition::IClient* client)
        {
            ASSERT(client != nullptr);
            if (client != nullptr) {

                const string name(client->Name());
                if (name.empty() == true) {
                    ASSERT(false);
                    TRACE(Trace::Information, (_T("Registration of a nameless client.")));
                } else {
                    _adminLock.Lock();

                    ClientDataContainer::iterator element (_clients.find(name));

                    if (element != _clients.end()) {
                        // as the old one may be dangling becayse of a crash let's remove that one, this is the most logical thing to do
                        ClientRevoked(element->second.clientInterface);

                        TRACE(Trace::Information, (_T("Replace client %s."), name.c_str()));
                    }
                    else {
                        TRACE(Trace::Information, (_T("Added client %s."), name.c_str()));

                    }

                    _clients.emplace(std::piecewise_construct,
                                         std::forward_as_tuple(name),
                                         std::forward_as_tuple(client, Resolution()));

                    for (auto&& index : _observers) {
                        index->Attached(name, client);
                    }

                    _adminLock.Unlock();

                    RecalculateZOrder(client); //note: do outside lock
                }
            }
        }

        void ClientRevoked(const IUnknown* client)
        {
            // note do not release by looking up the name, client might live in another process and the name call might fail if the connection is gone
            ASSERT(client != nullptr);

            _adminLock.Lock();
            auto it = _clients.begin();
            while ( (it != _clients.end()) && (it->second.clientInterface != client) ) { ++it; }

            if (it != _clients.end()) {
                TRACE(Trace::Information, (_T("Remove client %s."), it->first.c_str()));
                for (auto index : _observers) {
                    // note as we have the name here, we could more efficiently pass the name to the
                    // caller as it is not allowed to get it from the pointer passes, but we are going
                    // to restructure the interface anyway
                    index->Detached(it->first.c_str());
                }

                _clients.erase(it);
            }
            _adminLock.Unlock();

            TRACE(Trace::Information, (_T("Client detached completed")));
        }

        // on new client
        void RecalculateZOrder(Exchange::IComposition::IClient* client)
        {
            ASSERT(client != nullptr);
            client->ChangedZOrder(_clients.size());
        }

        void PlatformReady()
        {
            PluginHost::ISubSystem* subSystems(_service->SubSystems());
            ASSERT(subSystems != nullptr);
            if (subSystems != nullptr) {
                subSystems->Set(PluginHost::ISubSystem::PLATFORM, nullptr);
                subSystems->Set(PluginHost::ISubSystem::GRAPHICS, nullptr);
                subSystems->Release();
            }
        }

        Exchange::IComposition::Rectangle FindClientRectangle(const string& name) const
        {
            Exchange::IComposition::Rectangle rectangle = Exchange::IComposition::Rectangle();

            _adminLock.Lock();

            const ClientData* clientdata = FindClientData(name);

            if (clientdata != nullptr) {
                rectangle = clientdata->currentRectangle;
            }

            _adminLock.Unlock();

            return rectangle;
        }

        uint32_t SetClientRectangle(const string& name, const Exchange::IComposition::Rectangle& rectangle)
        {

            _adminLock.Lock();

            ClientData* clientdata = FindClientData(name);

            if (clientdata != nullptr) {
                clientdata->currentRectangle = rectangle;
            }

            _adminLock.Unlock();

            return (clientdata != nullptr ? Core::ERROR_NONE : Core::ERROR_FIRST_RESOURCE_NOT_FOUND);
        }

        IClient* FindClient(const string& name) const
        {
            IClient* client = nullptr;

            _adminLock.Lock();

            const ClientData* clientdata = FindClientData(name);

            if (clientdata != nullptr) {
                client = clientdata->clientInterface;
                ASSERT(client != nullptr);
                client->AddRef();
            }

            _adminLock.Unlock();

            return client;
        }

        const ClientData* FindClientData(const string& name) const
        {
            const ClientData* clientdata = nullptr;
            auto iterator = GetClientIterator(name);
            if (iterator != _clients.end()) {
                clientdata = &(iterator->second);
            }
            return clientdata;
        }

        ClientData* FindClientData(const string& name)
        {
            return const_cast<ClientData*>(static_cast<const CompositorImplementation&>(*this).FindClientData(name));
        }

        mutable Core::CriticalSection _adminLock;
        PluginHost::IShell* _service;
        Core::ProxyType<RPC::InvokeServer> _engine;
        ExternalAccess* _externalAccess;
        std::list<Exchange::IComposition::INotification*> _observers;
        ClientDataContainer _clients;
    };

    SERVICE_REGISTRATION(CompositorImplementation, 1, 0);

} // namespace Plugin
} // namespace WPEFramework
