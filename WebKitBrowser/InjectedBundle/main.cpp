#include "Module.h"

#include <WPE/WebKit.h>

#include <cstdio>
#include <memory>
#include <syslog.h>

#include "ClassDefinition.h"
#include "NotifyWPEFramework.h"
#include "Utils.h"
#include "WhiteListedOriginDomainsList.h"

#define DEVELOPMENT
#ifdef DEVELOPMENT
#include <iostream>
#include <fstream>
unsigned char *bundle_js = nullptr;
unsigned int bundle_js_len = 0;
#else
#include "bundle.js.h"
#endif

using namespace WPEFramework;
using JavaScript::ClassDefinition;
using WebKit::WhiteListedOriginDomainsList;

WKBundleRef g_Bundle;

namespace WPEFramework {
namespace WebKit {
namespace Utils {

WKBundleRef GetBundle() {
    return (g_Bundle);
}

} } }

static Core::NodeId GetConnectionNode()
{
    string nodeName;

    Core::SystemInfo::GetEnvironment(string(_T("COMMUNICATOR_CONNECTOR")), nodeName);

    return (Core::NodeId(nodeName.c_str()));
}

static class PluginHost {
private:
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

public:
    PluginHost()
        : _engine(Core::ProxyType<RPC::InvokeServerType<4, 2>>::Create(Core::Thread::DefaultStackSize()))
        , _comClient(Core::ProxyType<RPC::CommunicatorClient>::Create(GetConnectionNode(), Core::ProxyType<Core::IIPCServer>(_engine)))
    {
        _engine->Announcements(_comClient->Announcement());
    }
    ~PluginHost()
    {
        TRACE_L1("Destructing injected bundle stuff!!! [%d]", __LINE__);
        Deinitialize();
    }

public:
    void Initialize(WKBundleRef bundle)
    {
        // Due to the LXC container support all ID's get mapped. For the TraceBuffer, use the host given ID.
        Trace::TraceUnit::Instance().Open(Core::ProcessInfo().Id());

        Trace::TraceType<Trace::Information, &Core::System::MODULE_NAME>::Enable(true);

        // We have something to report back, do so...
        uint32_t result = _comClient->Open(RPC::CommunicationTimeOut);
        if (result != Core::ERROR_NONE) {
            TRACE(Trace::Error, (_T("Could not open connection to node %s. Error: %s"), _comClient->Source().RemoteId(), Core::NumberType<uint32_t>(result).Text()));
        }

        _whiteListedOriginDomainPairs = WhiteListedOriginDomainsList::RequestFromWPEFramework();

#ifdef DEVELOPMENT
        std::ifstream fin("/usr/share/WPEFramework/WebKitBrowser/bundle.js");
        if (fin.good()) {
            std::cerr << "Success to open bundle.js" << std::endl;
            string line;
            string bundleJS;
            while (!fin.eof()) {
                std::getline(fin, line);
                bundleJS.append(line);
                bundleJS.append("\n");
            }
            fin.close();
            bundle_js_len = bundleJS.length();
            bundle_js = (unsigned char*) malloc(bundle_js_len);
            memcpy(bundle_js, bundleJS.c_str(), bundle_js_len);
        }
#endif
    }

    void Deinitialize()
    {
        if (_comClient.IsValid() == true) {
            _comClient.Release();
        }

        Core::Singleton::Dispose();
#ifdef DEVELOPMENT
        if (bundle_js != nullptr) {
            free (bundle_js);
        }
#endif
    }

    void WhiteList(WKBundleRef bundle)
    {

        // Whitelist origin/domain pairs for CORS, if set.
        if (_whiteListedOriginDomainPairs) {
            _whiteListedOriginDomainPairs->AddWhiteListToWebKit(bundle);
        }
    }

private:
    Core::ProxyType<RPC::InvokeServerType<4, 2> > _engine;
    Core::ProxyType<RPC::CommunicatorClient> _comClient;

    // White list for CORS.
    std::unique_ptr<WhiteListedOriginDomainsList> _whiteListedOriginDomainPairs;

} _wpeFrameworkClient;

extern "C" {

__attribute__((destructor)) static void unload()
{
    _wpeFrameworkClient.Deinitialize();
}

// Adds class to JS world.
void InjectInJSWorld(ClassDefinition& classDef, WKBundleFrameRef frame, WKBundleScriptWorldRef scriptWorld)
{
    // @Zan: for how long should "ClassDefinition.staticFunctions" remain valid? Can it be
    // released after "JSClassCreate"?

    JSGlobalContextRef context = WKBundleFrameGetJavaScriptContextForWorld(frame, scriptWorld);

    ClassDefinition::FunctionIterator function = classDef.GetFunctions();
    uint32_t functionCount = function.Count();

    // We need an extra entry that we set to all zeroes, to signal end of data.
    // TODO: memleak.
    JSStaticFunction* staticFunctions = new JSStaticFunction[functionCount + 1];

    int index = 0;
    while (function.Next()) {
        staticFunctions[index++] = (*function)->BuildJSStaticFunction();
    }

    staticFunctions[functionCount] = { nullptr, nullptr, 0 };

    // TODO: memleak.
    JSClassDefinition* JsClassDefinition = new JSClassDefinition{
        0, // version
        kJSClassAttributeNone, //attributes
        classDef.GetClassName().c_str(), // className
        0, // parentClass
        nullptr, // staticValues
        staticFunctions, // staticFunctions
        nullptr, //initialize
        nullptr, //finalize
        nullptr, //hasProperty
        nullptr, //getProperty
        nullptr, //setProperty
        nullptr, //deleteProperty
        nullptr, //getPropertyNames
        nullptr, //callAsFunction
        nullptr, //callAsConstructor
        nullptr, //hasInstance
        nullptr, //convertToType
    };

    JSClassRef jsClass = JSClassCreate(JsClassDefinition);
    JSValueRef jsObject = JSObjectMake(context, jsClass, nullptr);
    JSClassRelease(jsClass);

    // @Zan: can we make extension name same as ClassName?
    JSStringRef extensionString = JSStringCreateWithUTF8CString(classDef.GetExtName().c_str());
    JSObjectSetProperty(context, JSContextGetGlobalObject(context), extensionString, jsObject,
        kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete, nullptr);
    JSStringRelease(extensionString);
}

static WKBundlePageLoaderClientV6 s_pageLoaderClient = {
    { 6, nullptr },
    nullptr, // didStartProvisionalLoadForFrame
    nullptr, // didReceiveServerRedirectForProvisionalLoadForFrame
    nullptr, // didFailProvisionalLoadWithErrorForFrame
    nullptr, // didCommitLoadForFrame
    // didFinishDocumentLoadForFrame
    [](WKBundlePageRef, WKBundleFrameRef frame, WKTypeRef *userData, const void *) {
        std::cerr << "didFinishDocumentLoadForFrame" << std::endl;

        JSGlobalContextRef context = WKBundleFrameGetJavaScriptContext(frame);
        JSObjectRef windowObj = JSContextGetGlobalObject(context);
        JSStringRef objectHookerName = JSStringCreateWithUTF8CString("objectHooker");
        JSValueRef objectHookerVal = JSObjectGetProperty(context, windowObj, objectHookerName, nullptr);
        if (JSValueIsObject(context, objectHookerVal)) {
            JSObjectRef objectHookerObj = JSValueToObject(context, objectHookerVal, nullptr);
            JSStringRef hookName = JSStringCreateWithUTF8CString("hook");
            JSValueRef hookVal = JSObjectGetProperty(context, objectHookerObj, hookName, nullptr);
            if (JSValueIsObject(context, hookVal)) {
                JSObjectRef hookObj = JSValueToObject(context, hookVal, nullptr);
                JSObjectCallAsFunction(context, hookObj, nullptr, 0, nullptr, nullptr);
            }
            JSStringRelease(hookName);
        }
        JSStringRelease(objectHookerName);
    },
    nullptr, // didFinishLoadForFrame
    nullptr, // didFailLoadWithErrorForFrame
    nullptr, // didSameDocumentNavigationForFrame
    nullptr, // didReceiveTitleForFrame
    nullptr, // didFirstLayoutForFrame
    nullptr, // didFirstVisuallyNonEmptyLayoutForFrame
    nullptr, // didRemoveFrameFromHierarchy
    nullptr, // didDisplayInsecureContentForFrame
    nullptr, // didRunInsecureContentForFrame
    // didClearWindowObjectForFrame
    [](WKBundlePageRef, WKBundleFrameRef frame, WKBundleScriptWorldRef scriptWorld, const void*) {
        std::cerr << "didClearWindowObjectForFrame" << std::endl;
        // Add JS classes to JS world.
        ClassDefinition::Iterator ite = ClassDefinition::GetClassDefinitions();
        while (ite.Next()) {
            InjectInJSWorld(*ite, frame, scriptWorld);
        }
        // Inject @alticast OIPF Objects and ObjecetHooker
        if (bundle_js && bundle_js_len > 0) {
            string bundleJS = string(reinterpret_cast<char *>(bundle_js), bundle_js_len);
            JSGlobalContextRef context = WKBundleFrameGetJavaScriptContext(frame);
            JSObjectRef windowObj = JSContextGetGlobalObject(context);
            JSStringRef evalString = JSStringCreateWithUTF8CString("eval");
            JSValueRef evalValue = JSObjectGetProperty(context, windowObj, evalString, nullptr);
            JSObjectRef evalObj = JSValueToObject(context, evalValue, nullptr);
            JSStringRef argumentString = JSStringCreateWithUTF8CString(bundleJS.c_str());
            JSValueRef arguments[1];
            arguments[0] = JSValueMakeString(context, argumentString);

            JSObjectCallAsFunction(context, evalObj, nullptr, 1, arguments, nullptr);

            JSStringRelease(evalString);
            JSStringRelease(argumentString);
        }
    },
    nullptr, // didCancelClientRedirectForFrame
    nullptr, // willPerformClientRedirectForFrame
    nullptr, // didHandleOnloadEventsForFrame
    nullptr, // didLayoutForFrame
    nullptr, // didNewFirstVisuallyNonEmptyLayout_unavailable
    nullptr, // didDetectXSSForFrame
    nullptr, // shouldGoToBackForwardListItem
    nullptr, // globalObjectIsAvailableForFrame
    nullptr, // willDisconnectDOMWindowExtensionFromGlobalObject
    nullptr, // didReconnectDOMWindowExtensionToGlobalObject
    nullptr, // willDestroyGlobalObjectForDOMWindowExtension
    nullptr, // didFinishProgress
    nullptr, // shouldForceUniversalAccessFromLocalURL
    nullptr, // didReceiveIntentForFrame_unavailable
    nullptr, // registerIntentServiceForFrame_unavailable
    nullptr, // didLayout
    nullptr, // featuresUsedInPage
    nullptr, // willLoadURLRequest
    nullptr, // willLoadDataRequest
};

static WKBundlePageUIClientV4 s_pageUIClient = {
    { 4, nullptr },
    nullptr, // willAddMessageToConsole
    nullptr, // willSetStatusbarText
    nullptr, // willRunJavaScriptAlert
    nullptr, // willRunJavaScriptConfirm
    nullptr, // willRunJavaScriptPrompt
    nullptr, // mouseDidMoveOverElement
    nullptr, // pageDidScroll
    nullptr, // unused1
    nullptr, // shouldGenerateFileForUpload
    nullptr, // generateFileForUpload
    nullptr, // unused2
    nullptr, // statusBarIsVisible
    nullptr, // menuBarIsVisible
    nullptr, // toolbarsAreVisible
    nullptr, // didReachApplicationCacheOriginQuota
    nullptr, // didExceedDatabaseQuota
    nullptr, // createPlugInStartLabelTitle
    nullptr, // createPlugInStartLabelSubtitle
    nullptr, // createPlugInExtraStyleSheet
    nullptr, // createPlugInExtraScript
    nullptr, // unused3
    nullptr, // unused4
    nullptr, // unused5
    nullptr, // didClickAutoFillButton
    //willAddDetailedMessageToConsole
    [](WKBundlePageRef page, WKConsoleMessageSource source, WKConsoleMessageLevel level, WKStringRef message, uint32_t lineNumber,
        uint32_t columnNumber, WKStringRef url, const void* clientInfo) {
        string messageString = WebKit::Utils::WKStringToString(message);

        const uint16_t maxStringLength = Trace::TRACINGBUFFERSIZE - 1;
        if (messageString.length() > maxStringLength) {
            messageString = messageString.substr(0, maxStringLength);
        }

        // TODO: use "Trace" classes for different levels.
        TRACE_GLOBAL(Trace::Information, (messageString));
    }
};

static WKBundleClientV1 s_bundleClient = {
    { 1, nullptr },
    // didCreatePage
    [](WKBundleRef bundle, WKBundlePageRef page, const void* clientInfo) {
        // Register page loader client, for javascript callbacks.
        WKBundlePageSetPageLoaderClient(page, &s_pageLoaderClient.base);

        // Register UI client, this one will listen to log messages.
        WKBundlePageSetUIClient(page, &s_pageUIClient.base);

        _wpeFrameworkClient.WhiteList(bundle);
    },
    nullptr, // willDestroyPage
    nullptr, // didInitializePageGroup
    nullptr, // didReceiveMessage
    nullptr, // didReceiveMessageToPage
};

// Declare module name for tracer.
MODULE_NAME_DECLARATION(BUILD_REFERENCE)

void WKBundleInitialize(WKBundleRef bundle, WKTypeRef)
{
    g_Bundle = bundle;

    _wpeFrameworkClient.Initialize(bundle);

    WKBundleSetClient(bundle, &s_bundleClient.base);
}
}
