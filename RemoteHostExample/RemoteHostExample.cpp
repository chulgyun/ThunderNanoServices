#include "RemoteHostExample.h"
#include "interfaces/IRemoteInvocation.h"

namespace WPEFramework {
namespace Plugin {

    SERVICE_REGISTRATION(RemoteHostExample, 1, 0);

    const string RemoteHostExample::Initialize(PluginHost::IShell* service) 
    {
        string errorMessage = "";

        Config config;
        config.FromString(service->ConfigLine());

        string name = config.Name.Value();
        uint32_t connectionId = 0;   

        // If remoteTarget is set to empty string, plugin will be initialized on a local machine.
        // If remoteTarget will be IP addres, implementaiton will be a proxy to real plugin on remote
        // machine
        string remoteTarget = config.SlaveAddress.Value();
        _implementation = service->Root<Exchange::IRemoteHostExample>(connectionId, Core::infinite, "RemoteHostExampleImpl", ~0, remoteTarget);

        if (_implementation != nullptr) {
            if (remoteTarget.empty() == true) {
                // code run only on plugin host
                _implementation->Initialize(service);
            } 
        }

        return errorMessage;
    }

    void RemoteHostExample::Deinitialize(PluginHost::IShell* service) 
    {
        printf("Remote host example deinitialize\n");

        if (_implementation != nullptr) {
            _implementation->Release();
            _implementation = nullptr;
        }
    }

    string RemoteHostExample::Information() const 
    {
        return (string());
    }
}
}