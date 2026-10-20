#include <CommonCrypto/CommonDigest.h>
static int zelda_cube_uniform_layout(NSString *path, NSString *key) {
    // The read footprint was checked on these complete generated sources, not just a shader name.
    const char *expected = [key isEqualToString:@"ec4f05739b0fbc91_b"] ?
        "2784018cbd1703fd6d4a289148a171aba3851e10e9cd7869cbfde1ae436d021f" :
        [key isEqualToString:@"4e370f8adcd93698_b"] ?
        "bd8bcc13ea852625c3e6a380df2ca5763a48fa16f5934dde727f1fcdbbb5f67d" :
        [key isEqualToString:@"caf33ef1b3566666_b"] ?
        "bff6536ca8fc8d1a713047471123c2d8a51e00ecc771a75bb5baeca18d7d85e8" :
        [key isEqualToString:@"704413ad2261cb23_b"] ?
        "eec2619676dc6f7f8e06012d0f8a15c5d26d06611c7f494858b2dc337868771e" :
        [key isEqualToString:@"48e3901f9b4eb169_b"] ?
        "142b8e5f7c0acd05cef98532a84a8d1b8c3b71156b8fabc13fc410c20fb2eb2d" :
        [key isEqualToString:@"c3960b272d349125_b"] ?
        "93817d124b16c87d611a3ce7f4776ea65f57b1018382ac0c3eca0ca554884d46" : NULL;
    if (!expected) return 0;
    NSData *source = [NSData dataWithContentsOfFile:path];
    if (!source || source.length > 262144) return 0;
    uint8_t digest[32]; char hex[65]; CC_SHA256(source.bytes, (CC_LONG)source.length, digest);
    for (unsigned i = 0; i < 32; i++) snprintf(hex + 2*i, 3, "%02x", digest[i]);
    if (strcmp(hex, expected)) return 0;
    return [key isEqualToString:@"ec4f05739b0fbc91_b"] || [key isEqualToString:@"4e370f8adcd93698_b"] ||
        [key isEqualToString:@"48e3901f9b4eb169_b"] ? 1 : 2;
}
