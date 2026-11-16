/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpn/api.h"
int main(void) {
    cpn_engine_handle engine=0;
    cpn_error error={0};error.struct_size=sizeof(error);
    if(cpn_abi_version()!=CPN_ABI_VERSION)return 1;
    if(cpn_engine_create(NULL,&engine,&error)!=CPN_OK)return 2;
    if(cpn_engine_destroy(engine,&error)!=CPN_OK)return 3;
    return 0;
}
