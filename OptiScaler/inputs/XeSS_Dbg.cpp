#include "pch.h"
#include "XeSS_Native_Dx12.h"
#include <proxies/XeSS_Proxy.h>
#include "xess_dbg.h"

xess_result_t hk_xessSelectNetworkModel(xess_context_handle_t hContext, xess_network_model_t network)
{
    if (XeSSNativeDx12::Owns(hContext)) {
        auto fn = XeSSProxy::GameRuntime().xessSelectNetworkModel;
        return fn ? fn(hContext, network) : XESS_RESULT_ERROR_UNSUPPORTED;
    }

    return XESS_RESULT_SUCCESS;
}

xess_result_t hk_xessStartDump(xess_context_handle_t hContext, const xess_dump_parameters_t* dump_parameters)
{
    if (XeSSNativeDx12::Owns(hContext)) {
        auto fn = XeSSProxy::GameRuntime().xessStartDump;
        return fn ? fn(hContext, dump_parameters) : XESS_RESULT_ERROR_UNSUPPORTED;
    }

    return XESS_RESULT_SUCCESS;
}
