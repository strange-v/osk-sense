#include <stdio.h>

#include "../lib/OskCrypto/src/CryptoSelfTest.h"

int main() {
    const unsigned failed = osk::crypto::selfTest();
    printf("self-test mask: 0x%03x (%s)\n", failed, failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
