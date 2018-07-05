/* XMRig
 * Copyright 2010      Jeff Garzik <jgarzik@pobox.com>
 * Copyright 2012-2014 pooler      <pooler@litecoinpool.org>
 * Copyright 2014      Lucas Jones <https://github.com/lucasjones>
 * Copyright 2014-2016 Wolf9466    <https://github.com/OhGodAPet>
 * Copyright 2016      Jay D Dee   <jayddee246@gmail.com>
 * Copyright 2017-2018 XMR-Stak    <https://github.com/fireice-uk>, <https://github.com/psychocrypt>
 * Copyright 2016-2018 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */


#include <stdlib.h>
#include <uv.h>
#include <cc/CCClient.h>
#include <cc/ControlCommand.h>


#include "api/Api.h"
#include "App.h"
#include "common/Console.h"
#include "common/log/Log.h"
#include "common/Platform.h"
#include "core/Config.h"
#include "core/Controller.h"
#include "Cpu.h"
#include "crypto/CryptoNight.h"
#include "net/Network.h"
#include "Summary.h"
#include "version.h"
#include "workers/Workers.h"


#ifndef XMRIG_NO_HTTPD
#   include "common/api/Httpd.h"
#endif


App *App::m_self = nullptr;



App::App(int argc, char **argv) :
    m_restart(false),
    m_console(nullptr),
    m_httpd(nullptr)
{
    m_self = this;

    m_controller = new xmrig::Controller();
    if (m_controller->init(argc, argv) != 0) {
        return;
    }

    if (!m_controller->config()->isBackground()) {
        m_console = new Console(this);
    }

    uv_signal_init(uv_default_loop(), &m_sigHUP);
    uv_signal_init(uv_default_loop(), &m_sigINT);
    uv_signal_init(uv_default_loop(), &m_sigTERM);
}


App::~App()
{
    uv_tty_reset_mode();

    delete m_console;
    delete m_controller;

#   ifndef XMRIG_NO_HTTPD
    delete m_httpd;
#   endif

#   ifndef XMRIG_NO_CC
    if (m_ccclient) {
      delete m_ccclient;
    }
#   endif
}


int App::exec()
{
    if (!m_controller->isReady()) {
        return 2;
    }

    uv_signal_start(&m_sigHUP,  App::onSignal, SIGHUP);
    uv_signal_start(&m_sigINT,  App::onSignal, SIGINT);
    uv_signal_start(&m_sigTERM, App::onSignal, SIGTERM);

    background();

    if (!CryptoNight::init(m_controller->config()->algorithm().algo())) {
        LOG_ERR("\"%s\" hash self-test failed.", m_controller->config()->algorithm().name());
        return 1;
    }

    Summary::print(m_controller);

    if (m_controller->config()->isDryRun()) {
        LOG_NOTICE("OK");
        return 0;
    }

#   ifndef XMRIG_NO_API
    Api::start(m_controller);
#   endif

#   ifndef XMRIG_NO_HTTPD
    m_httpd = new Httpd(
                m_controller->config()->apiPort(),
                m_controller->config()->apiToken(),
                m_controller->config()->isApiIPv6(),
                m_controller->config()->isApiRestricted()
                );

    m_httpd->start();
#   endif

#   ifndef XMRIG_NO_CC
    if (m_controller->config()->ccHost() && m_controller->config()->ccPort() > 0) {
        uv_async_init(uv_default_loop(), &m_async, App::onCommandReceived);

        m_ccclient = new CCClient(m_controller, &m_async);

        if (!m_controller->config()->pools().front().isValid()) {
            LOG_WARN("No pool URL supplied, but CC server configured. Trying.");
        }
    } else {
        LOG_WARN("Please configure CC-Url and restart. CC feature is now deactivated.");
    }
#   endif

    m_controller->config()->oclInit();

    if (!Workers::start(m_controller)) {
        LOG_ERR("Failed to start threads");
        return 1;
    }

    m_controller->network()->connect();

    const int r = uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    uv_loop_close(uv_default_loop());

    return m_restart ? EINTR : r;
}


void App::onConsoleCommand(char command)
{
    switch (command) {
    case 'h':
    case 'H':
        Workers::printHashrate(true);
        break;

    case 'p':
    case 'P':
        if (Workers::isEnabled()) {
            LOG_INFO(m_controller->config()->isColors() ? "\x1B[01;33mpaused\x1B[0m, press \x1B[01;35mr\x1B[0m to resume" : "paused, press 'r' to resume");
            Workers::setEnabled(false);
        }
        break;

    case 'r':
    case 'R':
        if (!Workers::isEnabled()) {
            LOG_INFO(m_controller->config()->isColors() ? "\x1B[01;32mresumed" : "resumed");
            Workers::setEnabled(true);
        }
        break;

    case 3:
        LOG_WARN("Ctrl+C received, exiting");
        shutdown();
        break;

    default:
        break;
    }
}

void App::stop(bool restart)
{
    m_restart = restart;

    m_controller->network()->stop();
    Workers::stop();

    uv_stop(uv_default_loop());
}

void App::restart()
{
    m_self->stop(true);
}

void App::shutdown()
{
    m_self->stop(false);
}


void App::onSignal(uv_signal_t *handle, int signum)
{
    switch (signum)
    {
    case SIGHUP:
        LOG_WARN("SIGHUP received, exiting");
        break;

    case SIGTERM:
        LOG_WARN("SIGTERM received, exiting");
        break;

    case SIGINT:
        LOG_WARN("SIGINT received, exiting");
        break;

    default:
        break;
    }

    uv_signal_stop(handle);
    m_self->shutdown();
}


void App::onCommandReceived(uv_async_t* async)
{
    auto command = reinterpret_cast<ControlCommand::Command &> (async->data);
    switch (command) {
        case ControlCommand::START:
            Workers::setEnabled(true);
            break;
        case ControlCommand::STOP:
            Workers::setEnabled(false);
            break;
        case ControlCommand::UPDATE_CONFIG:;
        case ControlCommand::RESTART:
            App::restart();
            break;
        case ControlCommand::SHUTDOWN:
            App::shutdown();
            break;
        case ControlCommand::PUBLISH_CONFIG:;
            break;
    }
}
