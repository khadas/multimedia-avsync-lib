/*
 * Copyright (C) 2021 Amlogic Corporation.
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
#ifndef AML_AVSYNC_PCR_MONITOR_H
#define AML_AVSYNC_PCR_MONITOR_H

struct pcr_info {
    long long pts; //unit as us
    long long monoclk; //unint as us
};

enum pcr_monitor_status {
    UNINITIALIZE = 0,
    RECORDING,
    CONSTRUCT_MONITOR,
    WAIT_DEVIATION_STABLE,
    DEVIATION_READY,
    DEVIATION_LONG_TERM_READY,
};

int pcr_monitor_init(void ** monitor_handle);
int pcr_monitor_process(void *monitor_handle, struct pcr_info *pcr);
enum pcr_monitor_status pcr_monitor_get_status(void *monitor_handle);
int pcr_monitor_get_deviation(void *monitor_handle, int *ppm);
int pcr_monitor_destroy(void *monitor_handle);

#endif
