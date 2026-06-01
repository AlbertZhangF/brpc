# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The ASF licenses this file to You under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License.  You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# BUILD file for liburing (io_uring userspace library).
# Used via new_local_repository pointing to a local liburing source tree.

licenses(["notice"])  # MIT / GPL-2.0-only (dual-licensed)

cc_library(
    name = "liburing",
    srcs = [
        "src/queue.c",
        "src/register.c",
        "src/setup.c",
        "src/syscall.c",
    ],
    hdrs = glob([
        "src/include/**/*.h",
        "src/include/*.h",
    ]),
    includes = [
        "src/include",
    ],
    copts = [
        "-D_GNU_SOURCE",
        # Suppress warnings from third-party code
        "-Wno-unused-variable",
        "-Wno-implicit-function-declaration",
    ],
    visibility = ["//visibility:public"],
)
