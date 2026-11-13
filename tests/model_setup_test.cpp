#include "protocol.h"
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
using namespace lemonvox;
static std::vector<uint8_t> bytes(const std::string &s) { return {s.begin(), s.end()}; }
int main() {
    ProtocolConfiguration config{{"http://localhost:13305/api/v1",true,true,true,true,true},{},{},{}};
    ProtocolInventory catalog;
    auto json=bytes(R"({"data":[{"id":"asr","downloaded":false,"recipe":"whispercpp","labels":["audio","transcription"],"size":0.5},{"id":"chat","downloaded":true,"recipe":"llamacpp","labels":["chat"]},{"id":"bad","downloaded":false,"recipe":"embedding","labels":["embedding"]}]})");
    assert(Protocol::parse_catalog(json,catalog)==LV_PROTOCOL_OK);
    assert(catalog.models.size()==3);
    assert(Protocol::model_setup(catalog,"",LV_MODEL_ASR,true,true).state==LV_SETUP_NO_SELECTION);
    assert(Protocol::model_setup(catalog,"asr",LV_MODEL_ASR,true,true).can_install);
    assert(Protocol::model_setup(catalog,"asr",LV_MODEL_ASR,false,true).state==LV_SETUP_SERVER_UNAVAILABLE);
    assert(Protocol::model_setup(catalog,"asr",LV_MODEL_ASR,true,false).state==LV_SETUP_EXTERNAL_SERVER);
    assert(Protocol::model_setup(catalog,"bad",LV_MODEL_ASR,true,true).state==LV_SETUP_INCOMPATIBLE);
    TransportRequest request;
    assert(Protocol::make_install_request(config,catalog,"asr",LV_MODEL_ASR,true,false,request)!=LV_PROTOCOL_OK);
    assert(Protocol::make_install_request(config,catalog,"unknown",LV_MODEL_ASR,true,true,request)!=LV_PROTOCOL_OK);
    assert(Protocol::make_install_request(config,catalog,"asr",LV_MODEL_ASR,true,true,request)==LV_PROTOCOL_OK);
    assert(request.url=="http://localhost:13305/api/v1/pull");
    assert(Protocol::supports_model_management(bytes(R"({"version":"2026.46.2"})")));
    assert(!Protocol::supports_model_management(bytes(R"({"version":"9.3.0"})")));
    assert(!Protocol::supports_model_management(bytes(R"({"version":"2026.46.1"})")));
    assert(Protocol::catalog_apply_health(catalog,bytes(R"({"all_models_loaded":[]})"))==LV_PROTOCOL_OK);
    assert(Protocol::model_setup(catalog,"chat",LV_MODEL_CLEANUP,true,true).state==LV_SETUP_INSTALLED_NOT_LOADED);
    assert(Protocol::catalog_apply_health(catalog,bytes(R"({"all_models_loaded":[{"model_name":"chat","loaded":true}]})"))==LV_PROTOCOL_OK);
    assert(Protocol::model_setup(catalog,"chat",LV_MODEL_CLEANUP,true,true).state==LV_SETUP_READY);
    auto progress=Protocol::parse_install_progress_for_model(bytes(R"([{"id":"model:asr","type":"model","model_name":"asr","status":"downloading","running":true,"percent":42}])"),"asr");
    assert(progress.status==LV_PROTOCOL_OK && progress.percent==42 && progress.running && !progress.complete);
    progress=Protocol::parse_install_progress_for_model(bytes(R"([{"id":"model:asr","type":"model","model_name":"asr","status":"completed","running":false,"complete":true}])"),"asr");
    assert(progress.complete);
    progress=Protocol::parse_install_progress_for_model(bytes(R"([{"id":"model:asr","type":"model","model_name":"asr","status":"cancelled","running":true}])"),"asr");
    assert(progress.running && !progress.canceled);
    progress=Protocol::parse_install_progress_for_model(bytes("[]"),"asr");
    assert(progress.status==LV_PROTOCOL_OK && !progress.complete);
    assert(Protocol::parse_install_progress_for_model(bytes("{}"),"asr").status==LV_PROTOCOL_RESPONSE);
    // Catalog ambiguity and unsafe identifiers fail closed before any request.
    ProtocolInventory invalid;
    assert(Protocol::parse_catalog(bytes(R"({"data":[{"id":"same"},{"id":"same"}]})"),invalid)==LV_PROTOCOL_RESPONSE);
    for (const auto &id : {"../secret", "a;b", "a\\\"b", "a\nb", "$(touch /tmp/nope)"}) {
        auto bad = catalog;
        bad.models.push_back({id,true,false,false,false,false,false,0});
        assert(Protocol::make_install_request(config,bad,id,LV_MODEL_ASR,true,true,request)!=LV_PROTOCOL_OK);
    }
    auto ambiguous=catalog; ambiguous.models.push_back(catalog.models.front());
    assert(Protocol::make_install_request(config,ambiguous,"asr",LV_MODEL_ASR,true,true,request)!=LV_PROTOCOL_OK);
    assert(Protocol::make_install_request(config,catalog,"asr",LV_MODEL_ASR,false,true,request)!=LV_PROTOCOL_OK);
    assert(Protocol::make_install_request(config,catalog,"chat",LV_MODEL_CLEANUP,true,true,request)!=LV_PROTOCOL_OK);
    assert(Protocol::make_cancel_install_request(config,catalog,"asr",LV_MODEL_ASR,true,request)==LV_PROTOCOL_OK);
    assert(request.url=="http://localhost:13305/api/v1/downloads/control");
    assert(std::string(request.body.begin(),request.body.end())==R"({"id":"model:asr","action":"cancel"})");
    assert(Protocol::make_catalog_request(config,request)==LV_PROTOCOL_OK);
    assert(request.method==HttpMethod::get && request.url=="http://localhost:13305/api/v1/models?show_all=true");
    // A health check, a catalog read and ordinary inference validation never create a pull request.
    auto installedOnly=catalog;
    auto dictation=config; dictation.asr_model="asr"; dictation.cleanup_model="chat"; dictation.cleanup_prompt="Clean up text";
    assert(Protocol::validate_models(json,dictation)==LV_PROTOCOL_MODEL_UNAVAILABLE);
    auto installedJSON=std::string(json.begin(),json.end());
    installedJSON.replace(installedJSON.find("false"),5,"true");
    assert(Protocol::validate_models(bytes(installedJSON),dictation)==LV_PROTOCOL_OK);
    progress=Protocol::parse_install_progress_for_model(bytes(R"({"id":"model:asr","status":"error","running":false,"error":"token SECRET http://private/?key=x disk full permission denied"})"),"asr");
    assert(progress.failed && !progress.complete && progress.message.find("SECRET")==std::string::npos);
    assert(progress.message.find("disk space")!=std::string::npos && progress.message.find("permissions")!=std::string::npos);
    progress=Protocol::parse_install_progress_for_model(bytes(R"({"id":"model:asr","status":"cancelled","running":false})"),"asr");
    assert(progress.canceled && !progress.running);
    assert(Protocol::parse_install_progress_for_model(bytes(R"([{"id":"model:asr","status":"downloading","percent":101}])"),"asr").status==LV_PROTOCOL_RESPONSE);
    assert(Protocol::parse_install_progress_for_model(bytes(R"([{"id":"model:asr","status":"downloading"},{"id":"model:asr","status":"completed"}])"),"asr").status==LV_PROTOCOL_RESPONSE);
    assert(Protocol::parse_install_progress_for_model(bytes(R"([{"id":"other","status":true}])"),"asr").status==LV_PROTOCOL_RESPONSE);
    assert(!Protocol::supports_model_management(bytes(R"({"version":"2026.99.9"})")));
    assert(!Protocol::supports_model_management(bytes(R"({"version":"2026.46.2-beta"})")));
    assert(Protocol::catalog_apply_health(installedOnly,bytes(R"({"all_models_loaded":[{"model_name":5}]})"))==LV_PROTOCOL_RESPONSE);
    assert(Protocol::model_setup(installedOnly,"chat",LV_MODEL_CLEANUP,true,true).state==LV_SETUP_READY);

    assert(Protocol::parse_install_progress_for_model(bytes(R"({"id":"model:asr","status":"unknown"})"),"asr").status==LV_PROTOCOL_RESPONSE);
    assert(Protocol::parse_install_progress_for_model(bytes(R"({"id":"model:asr","status":"error","complete":true})"),"asr").status==LV_PROTOCOL_RESPONSE);
    assert(Protocol::parse_install_progress_for_model(bytes(R"({"id":"model:asr","model_name":"other","status":"downloading"})"),"asr").status==LV_PROTOCOL_RESPONSE);

    assert(Protocol::parse_install_progress_for_model(bytes(R"([{"id":"model:a","status":"downloading"},{"id":"model:b","status":"completed"}])"),"").status==LV_PROTOCOL_OK);

}
