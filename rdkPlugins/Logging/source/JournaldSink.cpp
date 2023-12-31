/*
* If not stated otherwise in this file or this component's LICENSE file the
* following copyright and licenses apply:
*
* Copyright 2022 Sky UK
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "JournaldSink.h"

#include <unistd.h>
#include <strings.h>
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <algorithm>

#include <systemd/sd-journal.h>

#include <Logging.h>

JournaldSink::JournaldSink(const std::string &containerId, std::shared_ptr<rt_dobby_schema> &containerConfig)
    : mContainerConfig(containerConfig),
      mContainerId(containerId),
      mBuf{}
{
    // Create a file descriptor we can write to
    // journald will handle line breaks etc automatically
    AI_LOG_FN_ENTRY();

    int logPriority = LOG_INFO;
    if (mContainerConfig->rdk_plugins->logging->data->journald_options)
    {
        std::string priority = mContainerConfig->rdk_plugins->logging->data->journald_options->priority;
        if (!priority.empty())
        {
            const std::map<std::string, int> options =
                {
                    {"LOG_EMERG", 0},
                    {"LOG_ALERT", 1},
                    {"LOG_CRIT", 2},
                    {"LOG_ERR", 3},
                    {"LOG_WARNING", 4},
                    {"LOG_NOTICE", 5},
                    {"LOG_INFO", 6},
                    {"LOG_DEBUG", 7}};

            auto it = options.find(priority);
            if (it != options.end())
            {
                logPriority = it->second;
            }
            else
            {
                AI_LOG_WARN("Could not parse journald priority - using LOG_INFO");
            }
        }
    }

    mJournaldSteamFd = sd_journal_stream_fd(mContainerId.c_str(), logPriority, 1);
    if (mJournaldSteamFd < 0)
    {
        AI_LOG_SYS_ERROR(-mJournaldSteamFd, "Failed to create journald stream fd");

        // Just use /dev/null instead
        mJournaldSteamFd = open("/dev/null", O_CLOEXEC | O_WRONLY);
    }

    AI_LOG_FN_EXIT();
}

JournaldSink::~JournaldSink()
{
    if (mJournaldSteamFd > 0)
    {
        if (close(mJournaldSteamFd) < 0)
        {
            AI_LOG_SYS_ERROR(errno, "Failed to close journald stream");
        }
    }
}

void JournaldSink::DumpLog(const int bufferFd)
{
    memset(mBuf, 0, sizeof(mBuf));

    std::lock_guard<std::mutex> locker(mLock);

    ssize_t ret;
    while (true)
    {
        ret = read(bufferFd, mBuf, sizeof(mBuf));
        if (ret <= 0)
        {
            break;
        }

        write(mJournaldSteamFd, mBuf, ret);
    }
}

void JournaldSink::process(const std::shared_ptr<AICommon::IPollLoop> &pollLoop, epoll_event event)
{
    std::lock_guard<std::mutex> locker(mLock);

    // Got some data, yay
    if (event.events & EPOLLIN)
    {
        ssize_t ret;
        memset(mBuf, 0, sizeof(mBuf));

        while (true)
        {
            ret = TEMP_FAILURE_RETRY(read(event.data.fd, mBuf, sizeof(mBuf)));
            if (ret < 0)
            {
                // We've reached the end of the data we can read so we're done here
                if (errno == EWOULDBLOCK)
                {
                    return;
                }

                // Something went wrong whilst reading
                AI_LOG_SYS_ERROR(errno, "Read from container %s tty failed", mContainerId.c_str());
                return;
            }

            if (write(mJournaldSteamFd, mBuf, ret) < 0)
            {
                AI_LOG_SYS_ERROR(errno, "Write to journald stream failed");
            }
        }

        return;
    }
    else if (event.events & EPOLLHUP)
    {
        pollLoop->delSource(shared_from_this(), event.data.fd);

        // Clean up - close the ptty fd
        if (close(event.data.fd) != 0)
        {
            AI_LOG_SYS_ERROR(errno, "Failed to close container ptty fd %d", event.data.fd);
        }
    }

    // Don't handle any other events
    return;
}