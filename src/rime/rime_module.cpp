#include "rime_predictor.h"

#include <rime/registry.h>
#include <rime/service.h>
#include <rime_api.h>

namespace {
class PredictorComponent final : public rime::Processor::Component {
public:
    rime::Processor *Create(const rime::Ticket &ticket) override {
        const auto userDirectory = rime::Service::instance().deployer().user_data_dir.to_utf8_string();
        return new rome::RimePredictor(ticket, rome::createSquirrelHost(userDirectory));
    }
};
}

static void rime_rome_ai_predict_initialize() {
    rime::Registry::instance().Register("rome_ai_predictor", new PredictorComponent);
}

static void rime_rome_ai_predict_finalize() {
    rime::Registry::instance().Unregister("rome_ai_predictor");
}

RIME_REGISTER_MODULE(rome_ai_predict)
