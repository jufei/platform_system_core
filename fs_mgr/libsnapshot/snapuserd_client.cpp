/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <arpa/inet.h>
#include <cutils/sockets.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>

#include <android-base/logging.h>
#include <libsnapshot/snapuserd_client.h>

namespace android {
namespace snapshot {

bool SnapuserdClient::ConnectToServerSocket(std::string socketname) {
    sockfd_ = 0;

    sockfd_ =
            socket_local_client(socketname.c_str(), ANDROID_SOCKET_NAMESPACE_RESERVED, SOCK_STREAM);
    if (sockfd_ < 0) {
        LOG(ERROR) << "Failed to connect to " << socketname;
        return false;
    }

    std::string msg = "query";

    int sendRet = Sendmsg(msg.c_str(), msg.size());
    if (sendRet < 0) {
        LOG(ERROR) << "Failed to send query message to snapuserd daemon with socket " << socketname;
        DisconnectFromServer();
        return false;
    }

    std::string str = Receivemsg();

    if (str.find("fail") != std::string::npos) {
        LOG(ERROR) << "Failed to receive message from snapuserd daemon with socket " << socketname;
        DisconnectFromServer();
        return false;
    }

    // If the daemon is passive then fallback to secondary active daemon. Daemon
    // is passive during transition phase. Please see RestartSnapuserd()
    if (str.find("passive") != std::string::npos) {
        LOG(DEBUG) << "Snapuserd is passive with socket " << socketname;
        DisconnectFromServer();
        return false;
    }

    CHECK(str.find("active") != std::string::npos);

    return true;
}

bool SnapuserdClient::ConnectToServer() {
    if (ConnectToServerSocket(GetSocketNameFirstStage())) return true;

    if (ConnectToServerSocket(GetSocketNameSecondStage())) return true;

    return false;
}

int SnapuserdClient::Sendmsg(const char* msg, size_t size) {
    int numBytesSent = TEMP_FAILURE_RETRY(send(sockfd_, msg, size, 0));
    if (numBytesSent < 0) {
        LOG(ERROR) << "Send failed " << strerror(errno);
        return -1;
    }

    if ((uint)numBytesSent < size) {
        LOG(ERROR) << "Partial data sent " << strerror(errno);
        return -1;
    }

    return 0;
}

std::string SnapuserdClient::Receivemsg() {
    int ret;
    struct timeval tv;
    fd_set set;
    char msg[PACKET_SIZE];
    std::string msgStr("fail");

    tv.tv_sec = 2;
    tv.tv_usec = 0;
    FD_ZERO(&set);
    FD_SET(sockfd_, &set);
    ret = select(sockfd_ + 1, &set, NULL, NULL, &tv);
    if (ret == -1) {  // select failed
        LOG(ERROR) << "Snapuserd:client: Select call failed";
    } else if (ret == 0) {  // timeout
        LOG(ERROR) << "Snapuserd:client: Select call timeout";
    } else {
        ret = TEMP_FAILURE_RETRY(recv(sockfd_, msg, PACKET_SIZE, 0));
        if (ret < 0) {
            PLOG(ERROR) << "Snapuserd:client: recv failed";
        } else if (ret == 0) {
            LOG(DEBUG) << "Snapuserd:client disconnected";
        } else {
            msgStr.clear();
            msgStr = msg;
        }
    }
    return msgStr;
}

int SnapuserdClient::StopSnapuserd(bool firstStageDaemon) {
    if (firstStageDaemon) {
        sockfd_ = socket_local_client(GetSocketNameFirstStage().c_str(),
                                      ANDROID_SOCKET_NAMESPACE_RESERVED, SOCK_STREAM);
        if (sockfd_ < 0) {
            LOG(ERROR) << "Failed to connect to " << GetSocketNameFirstStage();
            return -1;
        }
    } else {
        if (!ConnectToServer()) {
            LOG(ERROR) << "Failed to connect to socket " << GetSocketNameSecondStage();
            return -1;
        }
    }

    std::string msg = "stop";

    int sendRet = Sendmsg(msg.c_str(), msg.size());
    if (sendRet < 0) {
        LOG(ERROR) << "Failed to send stop message to snapuserd daemon";
        return -1;
    }

    DisconnectFromServer();

    return 0;
}

int SnapuserdClient::StartSnapuserdaemon(std::string socketname) {
    int retry_count = 0;

    if (fork() == 0) {
        const char* argv[] = {"/system/bin/snapuserd", socketname.c_str(), nullptr};
        if (execv(argv[0], const_cast<char**>(argv))) {
            LOG(ERROR) << "Failed to exec snapuserd daemon";
            return -1;
        }
    }

    // snapuserd is a daemon and will never exit; parent can't wait here
    // to get the return code. Since Snapuserd starts the socket server,
    // give it some time to fully launch.
    //
    // Try to connect to server to verify snapuserd server is started
    while (retry_count < MAX_CONNECT_RETRY_COUNT) {
        if (!ConnectToServer()) {
            retry_count++;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        } else {
            close(sockfd_);
            return 0;
        }
    }

    LOG(ERROR) << "Failed to start snapuserd daemon";
    return -1;
}

int SnapuserdClient::StartSnapuserd() {
    if (StartSnapuserdaemon(GetSocketNameFirstStage()) < 0) return -1;

    return 0;
}

int SnapuserdClient::InitializeSnapuserd(std::string cow_device, std::string backing_device,
                                         std::string control_device) {
    int ret = 0;

    if (!ConnectToServer()) {
        LOG(ERROR) << "Failed to connect to server ";
        return -1;
    }

    std::string msg = "start," + cow_device + "," + backing_device + "," + control_device;

    ret = Sendmsg(msg.c_str(), msg.size());
    if (ret < 0) {
        LOG(ERROR) << "Failed to send message " << msg << " to snapuserd daemon";
        return -1;
    }

    std::string str = Receivemsg();

    if (str.find("fail") != std::string::npos) {
        LOG(ERROR) << "Failed to receive ack for " << msg << " from snapuserd daemon";
        return -1;
    }

    DisconnectFromServer();

    LOG(DEBUG) << "Snapuserd daemon initialized with " << msg;
    return 0;
}

/*
 * Transition from first stage snapuserd daemon to second stage daemon involves
 * series of steps viz:
 *
 * 1: Create new dm-user devices - This is done by libsnapshot
 *
 * 2: Spawn the new snapuserd daemon - This is the second stage daemon which
 * will start the server but the dm-user misc devices is not binded yet.
 *
 * 3: Vector to this function contains pair of cow_device and source device.
 *    Ex: {{system_cow,system_a}, {product_cow, product_a}, {vendor_cow,
 *    vendor_a}}. This vector will be populated by the libsnapshot.
 *
 * 4: Initialize the Second stage daemon passing the information from the
 * vector. This will bind the daemon with dm-user misc device and will be ready
 * to serve the IO. Up until this point, first stage daemon is still active.
 * However, client library will mark the first stage daemon as passive and hence
 * all the control message from hereon will be sent to active second stage
 * daemon.
 *
 * 5: Create new dm-snapshot table. This is done by libsnapshot. When new table
 * is created, kernel will issue metadata read once again which will be served
 * by second stage daemon. However, any active IO will still be served by first
 * stage daemon.
 *
 * 6: Swap the snapshot table atomically - This is done by libsnapshot. Once
 * the swapping is done, all the IO will be served by second stage daemon.
 *
 * 7: Stop the first stage daemon. After this point second stage daemon is
 * completely active to serve the IO and merging process.
 *
 */
int SnapuserdClient::RestartSnapuserd(std::vector<std::vector<std::string>>& vec) {
    // Connect to first-stage daemon and send a terminate-request control
    // message. This will not terminate the daemon but will mark the daemon as
    // passive.
    if (!ConnectToServer()) {
        LOG(ERROR) << "Failed to connect to server ";
        return -1;
    }

    std::string msg = "terminate-request";

    int sendRet = Sendmsg(msg.c_str(), msg.size());
    if (sendRet < 0) {
        LOG(ERROR) << "Failed to send message " << msg << " to snapuserd daemon";
        return -1;
    }

    std::string str = Receivemsg();

    if (str.find("fail") != std::string::npos) {
        LOG(ERROR) << "Failed to receive ack for " << msg << " from snapuserd daemon";
        return -1;
    }

    CHECK(str.find("success") != std::string::npos);

    DisconnectFromServer();

    // Start the new daemon
    if (StartSnapuserdaemon(GetSocketNameSecondStage()) < 0) {
        LOG(ERROR) << "Failed to start new daemon at socket " << GetSocketNameSecondStage();
        return -1;
    }

    LOG(DEBUG) << "Second stage Snapuserd daemon created successfully at socket "
               << GetSocketNameSecondStage();

    // Vector contains all the device information to be passed to the new
    // daemon. Note that the caller can choose to initialize separately
    // by calling InitializeSnapuserd() API as well. In that case, vector
    // should be empty
    for (int i = 0; i < vec.size(); i++) {
        std::string& cow_device = vec[i][0];
        std::string& base_device = vec[i][1];
        std::string& control_device = vec[i][2];

        InitializeSnapuserd(cow_device, base_device, control_device);
        LOG(DEBUG) << "Daemon initialized with " << cow_device << ", " << base_device << " and "
                   << control_device;
    }

    return 0;
}

}  // namespace snapshot
}  // namespace android
