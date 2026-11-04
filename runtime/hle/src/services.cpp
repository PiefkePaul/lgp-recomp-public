#include "lgp/hle/services.hpp"

namespace lgp::hle {

BaseServices register_base_services(ServiceManager& sm, const SystemConfig& config,
                                    const rt::ProcessConfig& process) {
    BaseServices out;
    out.applet = std::make_shared<AppletState>(config);
    register_am(sm, out.applet);
    register_set(sm, config);
    register_time(sm, config);
    register_csrng(sm, process);
    register_pctl(sm);
    register_acc(sm, config);
    ApplicationInfo app;
    app.program_id = process.program_id;
    register_quiet_services(sm, app);
    register_apm(sm, out.applet);
    register_vi(sm);
    register_hwopus(sm);
    register_nifm(sm);
    register_ldn(sm);
    register_friends(sm);
    register_sockets(sm);
    register_bcat(sm);
    return out;
}

}  // namespace lgp::hle
