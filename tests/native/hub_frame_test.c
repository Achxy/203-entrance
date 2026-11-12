#include "frame.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    uint8_t out[HUB_HEADER+HUB_MAX], payload[HUB_MAX];
    assert(hub_crc(0,(const uint8_t *)"123456789",9)==0xcbf43926);
    for (size_t n=0;n<=HUB_MAX;n++) {
        for (size_t i=0;i<n;i++) payload[i]=(uint8_t)(i*37+n);
        size_t length=hub_encode(out,DATA,0x12345678,payload,n);
        assert(length==n+HUB_HEADER);
        assert(fwrite(out,1,length,stdout)==length);
    }
    assert(hub_encode(out,DATA,1,payload,HUB_MAX+1)==0);
    return 0;
}
