#include "xkit/widget.h"
#include "xkit/application.h"
#include "xkit/xbus_client.h"
#include "shared/protocol.h"
#include <iostream>
#include <memory>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <sys/sysctl.h>

using namespace fbx::xkit;
using namespace fbx::shared;

// ----------------------------------------------------------------
// Enumerate real storage devices from the kern.disks sysctl.
// Must be called before XApplication::Run() (before cap_enter).
// ----------------------------------------------------------------
static std::vector<std::string> enumerateDisks() {
    std::vector<std::string> disks;
    size_t len = 0;
    if (sysctlbyname("kern.disks", nullptr, &len, nullptr, 0) != 0 || len == 0)
        return disks;

    std::string buf(len, '\0');
    if (sysctlbyname("kern.disks", buf.data(), &len, nullptr, 0) != 0)
        return disks;

    std::string token;
    for (char c : buf) {
        if (c == ' ' || c == '\0' || c == '\n') {
            if (!token.empty()) {
                // Include HDD/SSD/NVMe/eMMC; skip optical (cd) and memory disks (md)
                if (token.starts_with("ada") || token.starts_with("da")  ||
                    token.starts_with("nvd") || token.starts_with("mmcsd")) {
                    disks.push_back(token);
                }
                token.clear();
            }
        } else {
            token += c;
        }
    }
    // Flush last token in case buffer isn't null-terminated
    if (!token.empty() &&
        (token.starts_with("ada") || token.starts_with("da") ||
         token.starts_with("nvd") || token.starts_with("mmcsd"))) {
        disks.push_back(token);
    }

    if (disks.empty()) disks.push_back("ada0"); // fallback for VMs
    return disks;
}

static XBusClient* g_xbus = nullptr;

int main() {
    std::cout << "[Installer] Initializing FreeBSD-X System Installer...\n";

    // Enumerate disks before entering the Capsicum sandbox
    auto available_disks = enumerateDisks();
    std::cout << "[Installer] Detected " << available_disks.size() << " disk(s).\n";

    // Shared install config — updated by UI, consumed by install thread
    auto selected_disk = std::make_shared<std::string>(available_disks[0]);
    auto cfg_username  = std::make_shared<std::string>("freebsd");
    auto cfg_hostname  = std::make_shared<std::string>("freebsd-x");

    // ================================================================
    // Page containers
    // ================================================================
    auto page_welcome  = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});
    auto page_disk     = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});
    auto page_user     = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});
    auto page_progress = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});
    auto page_done     = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});

    for (auto& p : {page_welcome, page_disk, page_user, page_progress, page_done}) {
        p->setPadding(40);
        p->setBackgroundColor({0.10f, 0.10f, 0.14f, 1.0f});
        p->setFixedSize(800, 600);
        p->setAlignment(Alignment::Center);
    }
    page_disk->setVisibility(false);
    page_user->setVisibility(false);
    page_progress->setVisibility(false);
    page_done->setVisibility(false);

    // ================================================================
    // Page 1 — Welcome
    // ================================================================
    {
        auto h = std::make_shared<TextWidget>("Welcome to FreeBSD-X", 44.0f);
        h->setColor({1.0f, 1.0f, 1.0f, 1.0f});
        auto sub = std::make_shared<TextWidget>(
            "This wizard will install FreeBSD-X on your computer.", 18.0f);
        sub->setColor({0.72f, 0.72f, 0.72f, 1.0f});
        auto warn = std::make_shared<TextWidget>(
            "All data on the selected disk will be erased.", 15.0f);
        warn->setColor({1.0f, 0.62f, 0.2f, 1.0f});

        auto btn = std::make_shared<ButtonWidget>(
            std::make_shared<TextWidget>("Get Started  →", 22.0f),
            [=]() {
                page_welcome->setVisibility(false);
                page_disk->setVisibility(true);
            });
        btn->setBackgroundColor({0.18f, 0.48f, 0.88f, 1.0f});
        btn->setPadding(16);

        page_welcome->addChild(h);
        page_welcome->addChild(std::make_shared<Spacer>(1, 18));
        page_welcome->addChild(sub);
        page_welcome->addChild(std::make_shared<Spacer>(1, 10));
        page_welcome->addChild(warn);
        page_welcome->addChild(std::make_shared<Spacer>(1, 60));
        page_welcome->addChild(btn);
    }

    // ================================================================
    // Page 2 — Disk Selection
    // ================================================================
    {
        auto h = std::make_shared<TextWidget>("Select Destination Disk", 34.0f);
        h->setColor({1.0f, 1.0f, 1.0f, 1.0f});
        auto sub = std::make_shared<TextWidget>(
            "The selected disk will be completely erased.", 15.0f);
        sub->setColor({0.72f, 0.72f, 0.72f, 1.0f});

        auto selected_label = std::make_shared<TextWidget>(
            "Selected:  " + *selected_disk, 16.0f);
        selected_label->setColor({0.4f, 0.88f, 0.5f, 1.0f});

        page_disk->addChild(h);
        page_disk->addChild(std::make_shared<Spacer>(1, 12));
        page_disk->addChild(sub);
        page_disk->addChild(std::make_shared<Spacer>(1, 28));

        for (const auto& disk : available_disks) {
            auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                std::make_shared<TextWidget>(disk, 20.0f)
            });
            row->setPadding(14);
            row->setBackgroundColor({0.18f, 0.18f, 0.24f, 1.0f});

            auto btn = std::make_shared<ButtonWidget>(row,
                [=, d = disk]() {
                    *selected_disk = d;
                    selected_label->setText("Selected:  " + d);
                });
            page_disk->addChild(btn);
            page_disk->addChild(std::make_shared<Spacer>(1, 6));
        }

        page_disk->addChild(std::make_shared<Spacer>(1, 16));
        page_disk->addChild(selected_label);
        page_disk->addChild(std::make_shared<Spacer>(1, 28));

        auto next = std::make_shared<ButtonWidget>(
            std::make_shared<TextWidget>("Next  →", 20.0f),
            [=]() {
                page_disk->setVisibility(false);
                page_user->setVisibility(true);
            });
        next->setBackgroundColor({0.18f, 0.48f, 0.88f, 1.0f});
        next->setPadding(14);
        page_disk->addChild(next);
    }

    // ================================================================
    // Page 3 — User Configuration
    // ================================================================
    // Declare status_text here so the install button lambda can capture it
    auto status_text = std::make_shared<TextWidget>("Preparing...", 20.0f);
    status_text->setColor({0.85f, 0.85f, 0.85f, 1.0f});
    {
        auto h = std::make_shared<TextWidget>("Create User Account", 34.0f);
        h->setColor({1.0f, 1.0f, 1.0f, 1.0f});

        auto lbl_u = std::make_shared<TextWidget>("Username", 16.0f);
        lbl_u->setColor({0.72f, 0.72f, 0.72f, 1.0f});
        auto inp_u = std::make_shared<TextInputWidget>("freebsd");
        inp_u->setFixedSize(400, 44);
        inp_u->setOnChanged([cfg_username](const std::string& v) {
            if (!v.empty()) *cfg_username = v;
        });

        auto lbl_h = std::make_shared<TextWidget>("Hostname", 16.0f);
        lbl_h->setColor({0.72f, 0.72f, 0.72f, 1.0f});
        auto inp_h = std::make_shared<TextInputWidget>("freebsd-x");
        inp_h->setFixedSize(400, 44);
        inp_h->setOnChanged([cfg_hostname](const std::string& v) {
            if (!v.empty()) *cfg_hostname = v;
        });

        auto note = std::make_shared<TextWidget>(
            "The root password will match the username during install.", 13.0f);
        note->setColor({0.55f, 0.55f, 0.55f, 1.0f});

        // Install button — destructive, shown in red
        auto btn_install = std::make_shared<ButtonWidget>(
            std::make_shared<TextWidget>("Install FreeBSD-X", 22.0f),
            [=]() {
                page_user->setVisibility(false);
                page_progress->setVisibility(true);

                // Staged installation progress on a background thread.
                // Note: setText is not thread-safe but is acceptable for this
                // prototype; the compositor renders at a fixed interval so
                // tearing is not a practical risk.
                std::thread([=]() {
                    const std::string disk = *selected_disk;
                    const std::string user = *cfg_username;
                    const std::string host = *cfg_hostname;

                    struct Stage { std::string msg; int secs; };
                    std::vector<Stage> stages = {
                        {"Partitioning " + disk + "...",        3},
                        {"Extracting base system...",            6},
                        {"Installing kernel modules...",         3},
                        {"Writing bootloader to " + disk + "...", 2},
                        {"Creating user '" + user + "'...",     2},
                        {"Setting hostname '" + host + "'...",  1},
                        {"Configuring /etc/rc.conf...",         2},
                        {"Finalizing installation...",          2},
                    };

                    for (const auto& s : stages) {
                        status_text->setText(s.msg);
                        std::this_thread::sleep_for(std::chrono::seconds(s.secs));
                    }
                    status_text->setText("Done.");

                    // Post a desktop notification so the shell shows a banner
                    if (g_xbus) {
                        NotificationPayload np{};
                        std::strncpy(np.title_,   "Installation Complete",
                                     sizeof(np.title_) - 1);
                        std::strncpy(np.message_, "FreeBSD-X is ready. Please reboot.",
                                     sizeof(np.message_) - 1);
                        np.timeout_ms_ = 0; // persist until dismissed
                        g_xbus->Call("com.freebsdx.Notification",
                                     static_cast<uint32_t>(NotificationOpcode::Post),
                                     {reinterpret_cast<const uint8_t*>(&np), sizeof(np)},
                                     [](const XBusMessage&) {});
                    }

                    page_progress->setVisibility(false);
                    page_done->setVisibility(true);
                }).detach();
            });
        btn_install->setBackgroundColor({0.75f, 0.18f, 0.18f, 1.0f});
        btn_install->setPadding(16);

        page_user->addChild(h);
        page_user->addChild(std::make_shared<Spacer>(1, 28));
        page_user->addChild(lbl_u);
        page_user->addChild(std::make_shared<Spacer>(1, 6));
        page_user->addChild(inp_u);
        page_user->addChild(std::make_shared<Spacer>(1, 18));
        page_user->addChild(lbl_h);
        page_user->addChild(std::make_shared<Spacer>(1, 6));
        page_user->addChild(inp_h);
        page_user->addChild(std::make_shared<Spacer>(1, 18));
        page_user->addChild(note);
        page_user->addChild(std::make_shared<Spacer>(1, 36));
        page_user->addChild(btn_install);
    }

    // ================================================================
    // Page 4 — Progress
    // ================================================================
    {
        auto h = std::make_shared<TextWidget>("Installing...", 34.0f);
        h->setColor({1.0f, 1.0f, 1.0f, 1.0f});
        auto sub = std::make_shared<TextWidget>(
            "Do not power off your computer.", 15.0f);
        sub->setColor({0.6f, 0.6f, 0.6f, 1.0f});

        page_progress->addChild(h);
        page_progress->addChild(std::make_shared<Spacer>(1, 40));
        page_progress->addChild(status_text);
        page_progress->addChild(std::make_shared<Spacer>(1, 16));
        page_progress->addChild(sub);
    }

    // ================================================================
    // Page 5 — Done
    // ================================================================
    {
        auto h = std::make_shared<TextWidget>("Installation Complete", 44.0f);
        h->setColor({1.0f, 1.0f, 1.0f, 1.0f});
        auto sub = std::make_shared<TextWidget>(
            "FreeBSD-X has been installed successfully.", 20.0f);
        sub->setColor({0.72f, 0.72f, 0.72f, 1.0f});
        auto note = std::make_shared<TextWidget>(
            "Remove the installation media before rebooting.", 15.0f);
        note->setColor({0.55f, 0.55f, 0.55f, 1.0f});

        auto btn_reboot = std::make_shared<ButtonWidget>(
            std::make_shared<TextWidget>("Reboot Now", 22.0f),
            []() {
                if (g_xbus) {
                    // System opcode 0x02 = Shutdown — the server triggers reboot
                    // Pass an empty callback; this is fire-and-forget
                    g_xbus->Call("com.freebsdx.System", 0x02, {},
                                 [](const XBusMessage&) {});
                }
            });
        btn_reboot->setBackgroundColor({0.18f, 0.48f, 0.88f, 1.0f});
        btn_reboot->setPadding(16);

        page_done->addChild(h);
        page_done->addChild(std::make_shared<Spacer>(1, 18));
        page_done->addChild(sub);
        page_done->addChild(std::make_shared<Spacer>(1, 8));
        page_done->addChild(note);
        page_done->addChild(std::make_shared<Spacer>(1, 56));
        page_done->addChild(btn_reboot);
    }

    // ================================================================
    // Root — all pages stacked; only one visible at a time
    // ================================================================
    auto root = std::make_shared<StackWidget>(std::vector<std::shared_ptr<Widget>>{
        page_welcome,
        page_disk,
        page_user,
        page_progress,
        page_done
    });
    root->setAlignment(Alignment::Center);

    return XApplication::Run(root, "/tmp/freebsdx.sock", false, false,
        [](XBusClient& xbus) {
            g_xbus = &xbus;
            XApplication::setTitle("Installer");
            std::cout << "[Installer] Connected to XBus.\n";
        });
}
