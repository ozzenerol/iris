/* Shared by the runtime and the compiler, which writes constant floats into
   text the same way. The Makefile puts it into the runtime's source in place
   of its #include. */
#ifndef IRIS_RYU_H
#define IRIS_RYU_H
#include <stdbool.h>
#include <stdint.h>

/* The shortest digits that read back as a float, with Ryu (Ulf Adams,
   "Ryu: fast float-to-string conversion", PLDI 2018). The tables hold 5^i
   and 2^k / 5^i to 125 bits; the same ones serve f64 and f32. */
static const uint64_t ryuPow5Inv[292][2] = {
    {0x0000000000000001u, 0x2000000000000000u}, {0x999999999999999au, 0x1999999999999999u},
    {0x47ae147ae147ae15u, 0x147ae147ae147ae1u}, {0x6c8b4395810624deu, 0x10624dd2f1a9fbe7u},
    {0x7a786c226809d496u, 0x1a36e2eb1c432ca5u}, {0x61f9f01b866e43abu, 0x14f8b588e368f084u},
    {0xb4c7f34938583622u, 0x10c6f7a0b5ed8d36u}, {0x87a6520ec08d236au, 0x1ad7f29abcaf4857u},
    {0x9fb841a566d74f88u, 0x15798ee2308c39dfu}, {0xe62d01511f12a607u, 0x112e0be826d694b2u},
    {0xd6ae6881cb5109a4u, 0x1b7cdfd9d7bdbab7u}, {0xdef1ed34a2a73aeau, 0x15fd7fe17964955fu},
    {0x7f27f0f6e885c8bbu, 0x119799812dea1119u}, {0x650cb4be40d60df8u, 0x1c25c268497681c2u},
    {0xea70909833de7193u, 0x16849b86a12b9b01u}, {0x21f3a6e0297ec143u, 0x1203af9ee756159bu},
    {0x6985d7cd0f313537u, 0x1cd2b297d889bc2bu}, {0x2137dfd73f5a90f9u, 0x170ef54646d49689u},
    {0xe75fe645cc4873fau, 0x12725dd1d243aba0u}, {0xa5663d3c7a0d865du, 0x1d83c94fb6d2ac34u},
    {0x511e976394d79eb1u, 0x179ca10c9242235du}, {0xda7edf82dd794bc1u, 0x12e3b40a0e9b4f7du},
    {0x2a6498d1625bac68u, 0x1e392010175ee596u}, {0xeeb6e0a781e2f053u, 0x182db34012b25144u},
    {0x58924d52ce4f26a9u, 0x1357c299a88ea76au}, {0x27507bb7b07ea441u, 0x1ef2d0f5da7dd8aau},
    {0x52a6c95fc0655034u, 0x18c240c4aecb13bbu}, {0x0eebd44c99eaa690u, 0x13ce9a36f23c0fc9u},
    {0xb17953adc3110a80u, 0x1fb0f6be50601941u}, {0xc12ddc8b02740867u, 0x195a5efea6b34767u},
    {0x3424b06f3529a052u, 0x14484bfeebc29f86u}, {0x901d59f290ee19dbu, 0x1039d66589687f9eu},
    {0x4cfbc31db4b0295fu, 0x19f623d5a8a73297u}, {0x3d9635b15d59bab2u, 0x14c4e977ba1f5bacu},
    {0x97ab5e277de16228u, 0x109d8792fb4c4956u}, {0xf2abc9d8c9689d0du, 0x1a95a5b7f87a0ef0u},
    {0x5bbca17a3aba173eu, 0x154484932d2e725au}, {0xafca1ac82efb45cbu, 0x11039d428a8b8eaeu},
    {0xb2dcf7a6b1920945u, 0x1b38fb9daa78e44au}, {0xf57d92ebc141a104u, 0x15c72fb1552d836eu},
    {0xc46475896767b403u, 0x116c262777579c58u}, {0x6d6d88dbd8a5ecd2u, 0x1be03d0bf225c6f4u},
    {0x8abe071646eb23dbu, 0x164cfda3281e38c3u}, {0x6efe6c11d255b649u, 0x11d7314f534b609cu},
    {0xb197134fb6ef8a0eu, 0x1c8b821885456760u}, {0x27ac0f72f8bfa1a5u, 0x16d601ad376ab91au},
    {0xb95672c260994e1eu, 0x1244ce242c5560e1u}, {0xf5571e03cdc21695u, 0x1d3ae36d13bbce35u},
    {0x2aac18030b01ababu, 0x17624f8a762fd82bu}, {0xbbbce0026f348956u, 0x12b50c6ec4f31355u},
    {0x92c7ccd0b1eda889u, 0x1dee7a4ad4b81eefu}, {0xdbd30a408e57ba07u, 0x17f1fb6f10934bf2u},
    {0x7ca8d50071dfc806u, 0x1327fc58da0f6ff5u}, {0xfaa7bb33e9660cd6u, 0x1ea6608e29b24cbbu},
    {0x9552fc298784d711u, 0x18851a0b548ea3c9u}, {0xaaa8c9bad2d0ac0eu, 0x139dae6f76d88307u},
    {0xdddadc5e1e1aace3u, 0x1f62b0b257c0d1a5u}, {0x7e48b04b4b488a4fu, 0x191bc08eac9a4151u},
    {0xcb6d59d5d5d3a1d9u, 0x141633a556e1cddau}, {0x3c577b1177dc817bu, 0x1011c2eaabe7d7e2u},
    {0xc6f25e825960cf2au, 0x19b604aaaca62636u}, {0x6bf518684780a5bbu, 0x14919d5556eb51c5u},
    {0x232a79ed06008496u, 0x10747ddddf22a7d1u}, {0xd1dd8fe1a3340756u, 0x1a53fc9631d10c81u},
    {0xa7e4731ae8f66c45u, 0x150ffd44f4a73d34u}, {0x531d28e253f8569eu, 0x10d9976a5d52975du},
    {0xeb61db03b98d5762u, 0x1af5bf109550f22eu}, {0xbc4e48cfc7a445e8u, 0x159165a6ddda5b58u},
    {0x6371d3d96c836b20u, 0x11411e1f17e1e2adu}, {0x9f1c8628ad9f11cdu, 0x1b9b6364f3030448u},
    {0xe5b06b53be18db0bu, 0x1615e91d8f359d06u}, {0xeaf3890fcb4715a2u, 0x11ab20e472914a6bu},
    {0x44b8db4c7871bc37u, 0x1c45016d841baa46u}, {0x03c715d6c6c1635fu, 0x169d9abe03495505u},
    {0x3638de456bcde919u, 0x1217aefe69077737u}, {0x56c163a2461641c1u, 0x1cf2b1970e725858u},
    {0xdf011c81d1ab67ceu, 0x17288e1271f51379u}, {0x7f3416ce4155eca5u, 0x1286d80ec190dc61u},
    {0x6520247d3556476eu, 0x1da48ce468e7c702u}, {0xea801d30f7783925u, 0x17b6d71d20b96c01u},
    {0xbb99b0f3f92cfa84u, 0x12f8ac174d612334u}, {0x5f5c4e532847f739u, 0x1e5aacf215683854u},
    {0x7f7d0b75b9d32c2eu, 0x18488a5b44536043u}, {0x9930d5f7c7dc2358u, 0x136d3b7c36a919cfu},
    {0x8eb4898c72f9d226u, 0x1f152bf9f10e8fb2u}, {0x722a07a38f2e41b8u, 0x18ddbcc7f40ba628u},
    {0xc1bb394fa5be9afau, 0x13e497065cd61e86u}, {0x9c5ec2190930f7f6u, 0x1fd424d6faf030d7u},
    {0x49e56814075a5ff8u, 0x197683df2f268d79u}, {0x6e51201005e1e660u, 0x145ecfe5bf520ac7u},
    {0xf1da800cd181851au, 0x104bd984990e6f05u}, {0x4fc400148268d4f5u, 0x1a12f5a0f4e3e4d6u},
    {0xd96999aa01ed772bu, 0x14dbf7b3f71cb711u}, {0xadee1488018ac5bcu, 0x10aff95cc5b09274u},
    {0x497ceda668de092cu, 0x1ab328946f80ea54u}, {0x3aca57b853e4d424u, 0x155c2076bf9a5510u},
    {0x623b7960431d7683u, 0x1116805effaeaa73u}, {0x9d2bf566d1c8bd9eu, 0x1b5733cb32b110b8u},
    {0x7dbcc452416d647fu, 0x15df5ca28ef40d60u}, {0xcafd69db678ab6ccu, 0x117f7d4ed8c33de6u},
    {0xab2f0fc572778adfu, 0x1bff2ee48e052fd7u}, {0x88f273045b92d580u, 0x1665bf1d3e6a8cacu},
    {0xd3f528d049424466u, 0x11eaff4a98553d56u}, {0xb988414d4203a0a3u, 0x1cab3210f3bb9557u},
    {0x6139cdd76802e6e9u, 0x16ef5b40c2fc7779u}, {0xe761717920025254u, 0x125915cd68c9f92du},
    {0xa568b58e999d5086u, 0x1d5b561574765b7cu}, {0x5120913ee14aa6d2u, 0x177c44ddf6c515fdu},
    {0xa74d40ff1aa21f0eu, 0x12c9d0b1923744cau}, {0x0baece64f769cb4au, 0x1e0fb44f50586e11u},
    {0x3c8bd850c5ee3c3bu, 0x180c903f7379f1a7u}, {0xca0979da37f1c9c9u, 0x133d4032c2c7f485u},
    {0xa9a8c2f6bfe942dbu, 0x1ec866b79e0cba6fu}, {0x2153cf2bccba9be3u, 0x18a0522c7e709526u},
    {0x1aa9728970954982u, 0x13b374f06526ddb8u}, {0xf775840f1a88759du, 0x1f8587e7083e2f8cu},
    {0x5f9136727ba05e17u, 0x19379fec0698260au}, {0x1940f85b9619e4dfu, 0x142c7ff0054684d5u},
    {0xe100c6afab47ea4cu, 0x1023998cd1053710u}, {0xce67a44c453fdd47u, 0x19d28f47b4d524e7u},
    {0xd852e9d69dccb106u, 0x14a8729fc3ddb71fu}, {0x79dbee454b0a2738u, 0x1086c219697e2c19u},
    {0x295fe3a211a9d859u, 0x1a71368f0f30468fu}, {0xbab31c81a7bb137au, 0x15275ed8d8f36ba5u},
    {0x6228e39aec95a92fu, 0x10ec4be0ad8f8951u}, {0x9d0e38f7e0ef7517u, 0x1b13ac9aaf4c0ee8u},
    {0xb0d82d931a592a79u, 0x15a956e225d67253u}, {0x8d79be0f4847552eu, 0x11544581b7dec1dcu},
    {0x158f967eda0bbb7cu, 0x1bba08cf8c979c94u}, {0x77a611ff14d62f97u, 0x162e6d72d6dfb076u},
    {0xf951a7ff43de8c79u, 0x11bebdf578b2f391u}, {0xc21c3ffed2fdad8eu, 0x1c6463225ab7ec1cu},
    {0x01b0333242648ad8u, 0x16b6b5b5155ff017u}, {0x0159c28e9b83a246u, 0x122bc490dde659acu},
    {0xcef604175f3903a3u, 0x1d12d41afca3c2acu}, {0x725e69ac4c2d9c83u, 0x17424348ca1c9bbdu},
    {0xf5185489d68ae39cu, 0x129b69070816e2fdu}, {0xee8d540fbdab05c6u, 0x1dc574d80cf16b2fu},
    {0xbed77672fe226b05u, 0x17d12a4670c1228cu}, {0xff12c528cb4ebc04u, 0x130dbb6b8d674ed6u},
    {0xcb513b74787df9a0u, 0x1e7c5f127bd87e24u}, {0x090dc929f9fe614du, 0x18637f41fcad31b7u},
    {0xa0d7d42194cb810au, 0x1382cc34ca2427c5u}, {0x67bfb9cf5478ce77u, 0x1f37ad21436d0c6fu},
    {0x1fcc94a5dd2d71f9u, 0x18f9574dcf8a7059u}, {0x7fd6dd517dbdf4c7u, 0x13faac3e3fa1f37au},
    {0xffbe2ee8c92fee0bu, 0x1ff779fd329cb8c3u}, {0x6631bf20a0f324d6u, 0x1992c7fdc216fa36u},
    {0xb827cc1a1a5c1d78u, 0x14756ccb01abfb5eu}, {0x935309ae7b7ce460u, 0x105df0a267bcc918u},
    {0x1eeb42b0c594a099u, 0x1a2fe76a3f9474f4u}, {0xe58902270476e6e1u, 0x14f31f8832dd2a5cu},
    {0xb7a0ce859d2bebe7u, 0x10c27fa028b0eeb0u}, {0x59014a6f61dfdfd8u, 0x1ad0cc33744e4ab4u},
    {0xe0cdd525e7e64cadu, 0x1573d68f903ea229u}, {0x4d7177518651d6f1u, 0x11297872d9cbb4eeu},
    {0x7be8bee8d6e957e8u, 0x1b758d848fac54b0u}, {0xfcba3253df211320u, 0x15f7a46a0c89dd59u},
    {0x63c8284318e74280u, 0x1192e9ee706e4aaeu}, {0x060d0d3827d86a66u, 0x1c1e43171a4a1117u},
    {0x6b3da42cecad21ebu, 0x167e9c127b6e7412u}, {0x88fe1cf0bd574e56u, 0x11fee341fc585cdbu},
    {0x419694b462254a23u, 0x1ccb0536608d615fu}, {0x67abaa29e81dd4e9u, 0x1708d0f84d3de77fu},
    {0xb95621bb2017dd87u, 0x126d73f9d764b932u}, {0xc223692b668c95a5u, 0x1d7becc2f23ac1eau},
    {0xce82ba891ed6de1du, 0x179657025b6234bbu}, {0xa53562074bdf1818u, 0x12deac01e2b4f6fcu},
    {0x3b889cd87964f359u, 0x1e3113363787f194u}, {0xfc6d4a46c783f5e1u, 0x18274291c6065adcu},
    {0x30576e9f06032b1au, 0x13529ba7d19eaf17u}, {0x1a257dcb3cd1de90u, 0x1eea92a61c311825u},
    {0x481dfe3c30a7e540u, 0x18bba884e35a79b7u}, {0xd34b31c9c0865100u, 0x13c9539d82aec7c5u},
    {0x5211e942cda3b4cdu, 0x1fa885c8d117a609u}, {0x74db21023e1c90a4u, 0x19539e3a40dfb807u},
    {0xf715b401cb4a0d50u, 0x1442e4fb67196005u}, {0xf8de299b09080aa7u, 0x103583fc527ab337u},
    {0x8e304291a80cddd7u, 0x19ef3993b72ab859u}, {0x3e8d020e200a4b13u, 0x14bf6142f8eef9e1u},
    {0x653d9b3e80083c0fu, 0x10991a9bfa58c7e7u}, {0x6ec8f864000d2ce4u, 0x1a8e90f9908e0ca5u},
    {0x8bd3f9e999a423eau, 0x153eda614071a3b7u}, {0x3ca994bae1501cbbu, 0x10ff151a99f482f9u},
    {0xc775bac49bb3612bu, 0x1b31bb5dc320d18eu}, {0xd2c4956a16291a89u, 0x15c162b168e70e0bu},
    {0xdbd0778811ba7ba1u, 0x11678227871f3e6fu}, {0x2c80bf401c5d929bu, 0x1bd8d03f3e9863e6u},
    {0xbd33cc3349e47549u, 0x16470cff6546b651u}, {0xca8fd68f6e505dd4u, 0x11d270cc51055ea7u},
    {0x4419574be3b3c953u, 0x1c83e7ad4e6efdd9u}, {0x0347790982f63aa9u, 0x16cfec8aa52597e1u},
    {0xcf6c60d468c4fbbau, 0x123ff06eea847980u}, {0xe57a34870e07f92au, 0x1d331a4b10d3f59au},
    {0x512e906c0b399422u, 0x175c1508da432ae2u}, {0xda8ba6bcd5c7a9b5u, 0x12b010d3e1cf5581u},
    {0x90df712e22d90f87u, 0x1de6815302e5559cu}, {0xda4c5a8b4f140c6cu, 0x17eb9aa8cf1dde16u},
    {0xaea37ba2a5a9a38au, 0x1322e220a5b17e78u}, {0x7dd25f6aa2a905a9u, 0x1e9e369aa2b59727u},
    {0x97db7f888220d154u, 0x187e92154ef7ac1fu}, {0x797c6606ce80a777u, 0x139874ddd8c6234cu},
    {0x8f2d700ae4010bf1u, 0x1f5a549627a36badu}, {0x0c2459a25000d65au, 0x191510781fb5efbeu},
    {0x701d1481d99a4515u, 0x1410d9f9b2f7f2feu}, {0xc017439b147b6a77u, 0x100d7b2e28c65bfeu},
    {0xccf205c4ed9243f2u, 0x19af2b7d0e0a2ccau}, {0x0a5b37d0be0e9cc2u, 0x148c22ca71a1bd6fu},
    {0x0848f973cb3ee3ceu, 0x10701bd527b4978cu}, {0xda0e5bec78649fb0u, 0x1a4cf9550c5425acu},
    {0x7b3eaff060507fc0u, 0x150a6110d6a9b7bdu}, {0x95cbbff380406633u, 0x10d51a73deee2c97u},
    {0xefac665266cd7052u, 0x1aee90b964b04758u}, {0x2623850eb8a459dbu, 0x158ba6fab6f36c47u},
    {0x1e82d0d893b6ae49u, 0x113c85955f29236cu}, {0xfd9e1af41f8ab075u, 0x1b9408eefea838acu},
    {0x97b1af29b2d559f7u, 0x16100725988693bdu}, {0xac8e25baf5777b2cu, 0x11a66c1e139edc97u},
    {0x7a7d092b2258c513u, 0x1c3d79c9b8fe2dbfu}, {0x61fda0ef4ead6a76u, 0x169794a160cb57ccu},
    {0xe7fe1a590bbdeec5u, 0x1212dd4de7091309u}, {0xa6635d5b45fcb13au, 0x1ceafbafd80e84dcu},
    {0x851c4aaf6b308dc8u, 0x172262f3133ed0b0u}, {0xd0e36ef2bc26d7d4u, 0x1281e8c275cbda26u},
    {0xb49f17eac6a48c86u, 0x1d9ca79d894629d7u}, {0x2a18dfef0550706bu, 0x17b08617a104ee46u},
    {0x54e0b3259dd9f389u, 0x12f39e794d9d8b6bu}, {0x87cdeb6f62f65274u, 0x1e5297287c2f4578u},
    {0xd30b22bf825ea85du, 0x18421286c9bf6ac6u}, {0x0f3c1bcc684bb9e4u, 0x13680ed23aff889fu},
    {0x18602c7a4079296du, 0x1f0ce4839198da98u}, {0x46b356c833942124u, 0x18d71d360e13e213u},
    {0x388f78a029434db6u, 0x13df4a91a4dcb4dcu}, {0x5a7f2766a86baf8au, 0x1fcbaa82a1612160u},
    {0x153285ebb9efbfa2u, 0x196fbb9bb44db44du}, {0xaa8ed189618c994eu, 0x145962e2f6a4903du},
    {0xeed8a7a11ad6e10cu, 0x1047824f2bb6d9cau}, {0x7e27729b5e249b45u, 0x1a0c03b1df8af611u},
    {0xfe85f549181d4904u, 0x14d6695b193bf80du}, {0xcb9e5dd4134aa0d0u, 0x10ab877c142ff9a4u},
    {0xdf63c9535211014du, 0x1aac0bf9b9e65c3au}, {0x191ca10f74da6771u, 0x15566ffafb1eb02fu},
    {0xadb080d92a4852c1u, 0x1111f32f2f4bc025u}, {0x15e7348eaa0d5134u, 0x1b4feb7eb212cd09u},
    {0xab1f5d3eee710dc4u, 0x15d98932280f0a6du}, {0xbc1917658b8da49du, 0x117ad428200c0857u},
    {0x2cf4f23c127c3a94u, 0x1bf7b9d9cce00d59u}, {0xf0c3f4fcdb969543u, 0x165fc7e170b33de0u},
    {0x5a365d9716121103u, 0x11e6398126f5cb1au}, {0x9056fc24f01ce804u, 0x1ca38f350b22de90u},
    {0xd9df301d8ce3ecd0u, 0x16e93f5da2824ba6u}, {0xe17f59b13d8323dau, 0x125432b14ecea2ebu},
    {0x68cbc2b52f38395cu, 0x1d53844ee47dd179u}, {0x53d6355dbf602de3u, 0x177603725064a794u},
    {0xa9782ab165e68b1cu, 0x12c4cf8ea6b6ec76u}, {0x0f26aab56fd744fau, 0x1e07b27dd78b13f1u},
    {0x3f52222abfdf6a62u, 0x18062864ac6f4327u}, {0x65db4e88997f884eu, 0x1338205089f29c1fu},
    {0x6fc54a7428cc0d4au, 0x1ec033b40fea9365u}, {0x596aa1f68709a43bu, 0x1899c2f673220f84u},
    {0xadeee7f86c07b696u, 0x13ae3591f5b4d936u}, {0x497e3ff3e00c5756u, 0x1f7d228322baf524u},
    {0xd464fff64cd6ac45u, 0x1930e868e89590e9u}, {0x4383fff83d7889d1u, 0x14272053ed4473eeu},
    {0xcf9cccc69793a174u, 0x101f4d0ff1038ff1u}, {0x7f6147a425b90252u, 0x19cbae7fe805b31cu},
    {0xcc4dd2e9b7c7350fu, 0x14a2f1ffecd15c16u}, {0x3d0b0f215fd290d9u, 0x10825b3323dab012u},
    {0x61ab4b689950e7c1u, 0x1a6a2b85062ab350u}, {0x4e22a2ba1440b967u, 0x1521bc6a6b555c40u},
    {0x0b4ee894dd009453u, 0x10e7c9eebc4449cdu}, {0x1217da87c800ed51u, 0x1b0c764ac6d3a948u},
    {0xdb46486ca000bddau, 0x15a391d56bdc876cu}, {0x490506bd4ccd64afu, 0x114fa7ddefe39f8au},
    {0xa8080ac87ae23ab1u, 0x1bb2a62fe638ff43u}, {0x5339a239fbe82ef4u, 0x162884f31e93ff69u},
    {0x75c7b4fb2fecf25du, 0x11ba03f5b20fff87u}, {0x22d92191e647ea2eu, 0x1c5cd322b67fff3fu},
    {0xb57a8141850654f2u, 0x16b0a8e891ffff65u}, {0xc4620101373843f5u, 0x1226ed86db3332b7u},
    {0x3a366801f1f39feeu, 0x1d0b15a491eb8459u}, {0xfb5eb99b27f6198bu, 0x173c115074bc69e0u},
    {0x2f7efae2865e7ad6u, 0x129674405d6387e7u}, {0xe597f7d0d6fd9156u, 0x1dbd86cd6238d971u},
    {0x8479930d78cadaabu, 0x17cad23de82d7ac1u}, {0xd06142712d6f1556u, 0x1308a831868ac89au},
    {0x4d686a4eaf182222u, 0x1e74404f3daada91u}, {0xa453883ef279b4e8u, 0x185d003f6488aedau},
    {0xe9dc6cff28615d87u, 0x137d99cc506d58aeu}, {0xa960ae650d6895a4u, 0x1f2f5c7a1a488de4u},
    {0xbab3beb73ded4483u, 0x18f2b061aea07183u}, {0x2ef6322c318a9d36u, 0x13f559e7bee6c136u},
};
static const uint64_t ryuPow5[326][2] = {
    {0x0000000000000000u, 0x1000000000000000u}, {0x0000000000000000u, 0x1400000000000000u},
    {0x0000000000000000u, 0x1900000000000000u}, {0x0000000000000000u, 0x1f40000000000000u},
    {0x0000000000000000u, 0x1388000000000000u}, {0x0000000000000000u, 0x186a000000000000u},
    {0x0000000000000000u, 0x1e84800000000000u}, {0x0000000000000000u, 0x1312d00000000000u},
    {0x0000000000000000u, 0x17d7840000000000u}, {0x0000000000000000u, 0x1dcd650000000000u},
    {0x0000000000000000u, 0x12a05f2000000000u}, {0x0000000000000000u, 0x174876e800000000u},
    {0x0000000000000000u, 0x1d1a94a200000000u}, {0x0000000000000000u, 0x12309ce540000000u},
    {0x0000000000000000u, 0x16bcc41e90000000u}, {0x0000000000000000u, 0x1c6bf52634000000u},
    {0x0000000000000000u, 0x11c37937e0800000u}, {0x0000000000000000u, 0x16345785d8a00000u},
    {0x0000000000000000u, 0x1bc16d674ec80000u}, {0x0000000000000000u, 0x1158e460913d0000u},
    {0x0000000000000000u, 0x15af1d78b58c4000u}, {0x0000000000000000u, 0x1b1ae4d6e2ef5000u},
    {0x0000000000000000u, 0x10f0cf064dd59200u}, {0x0000000000000000u, 0x152d02c7e14af680u},
    {0x0000000000000000u, 0x1a784379d99db420u}, {0x0000000000000000u, 0x108b2a2c28029094u},
    {0x0000000000000000u, 0x14adf4b7320334b9u}, {0x4000000000000000u, 0x19d971e4fe8401e7u},
    {0x8800000000000000u, 0x1027e72f1f128130u}, {0xaa00000000000000u, 0x1431e0fae6d7217cu},
    {0xd480000000000000u, 0x193e5939a08ce9dbu}, {0xc9a0000000000000u, 0x1f8def8808b02452u},
    {0xbe04000000000000u, 0x13b8b5b5056e16b3u}, {0xad85000000000000u, 0x18a6e32246c99c60u},
    {0xd8e6400000000000u, 0x1ed09bead87c0378u}, {0x878fe80000000000u, 0x13426172c74d822bu},
    {0x6973e20000000000u, 0x1812f9cf7920e2b6u}, {0x03d0da8000000000u, 0x1e17b84357691b64u},
    {0x8262889000000000u, 0x12ced32a16a1b11eu}, {0x22fb2ab400000000u, 0x178287f49c4a1d66u},
    {0xabb9f56100000000u, 0x1d6329f1c35ca4bfu}, {0xcb54395ca0000000u, 0x125dfa371a19e6f7u},
    {0xbe2947b3c8000000u, 0x16f578c4e0a060b5u}, {0x2db399a0ba000000u, 0x1cb2d6f618c878e3u},
    {0xfc90400474400000u, 0x11efc659cf7d4b8du}, {0x7bb4500591500000u, 0x166bb7f0435c9e71u},
    {0xdaa16406f5a40000u, 0x1c06a5ec5433c60du}, {0xa8a4de8459868000u, 0x118427b3b4a05bc8u},
    {0xd2ce16256fe82000u, 0x15e531a0a1c872bau}, {0x87819baecbe22800u, 0x1b5e7e08ca3a8f69u},
    {0xf4b1014d3f6d5900u, 0x111b0ec57e6499a1u}, {0x71dd41a08f48af40u, 0x1561d276ddfdc00au},
    {0x0e549208b31adb10u, 0x1aba4714957d300du}, {0x28f4db456ff0c8eau, 0x10b46c6cdd6e3e08u},
    {0x33321216cbecfb24u, 0x14e1878814c9cd8au}, {0xbffe969c7ee839edu, 0x1a19e96a19fc40ecu},
    {0xf7ff1e21cf512434u, 0x105031e2503da893u}, {0xf5fee5aa43256d41u, 0x14643e5ae44d12b8u},
    {0x337e9f14d3eec892u, 0x197d4df19d605767u}, {0x005e46da08ea7ab6u, 0x1fdca16e04b86d41u},
    {0xa03aec4845928cb2u, 0x13e9e4e4c2f34448u}, {0xc849a75a56f72fdeu, 0x18e45e1df3b0155au},
    {0x7a5c1130ecb4fbd6u, 0x1f1d75a5709c1ab1u}, {0xec798abe93f11d65u, 0x13726987666190aeu},
    {0xa797ed6e38ed64bfu, 0x184f03e93ff9f4dau}, {0x517de8c9c728bdefu, 0x1e62c4e38ff87211u},
    {0xd2eeb17e1c7976b5u, 0x12fdbb0e39fb474au}, {0x87aa5ddda397d462u, 0x17bd29d1c87a191du},
    {0xe994f5550c7dc97bu, 0x1dac74463a989f64u}, {0x11fd195527ce9dedu, 0x128bc8abe49f639fu},
    {0xd67c5faa71c24568u, 0x172ebad6ddc73c86u}, {0x8c1b77950e32d6c2u, 0x1cfa698c95390ba8u},
    {0x57912abd28dfc639u, 0x121c81f7dd43a749u}, {0xad75756c7317b7c8u, 0x16a3a275d494911bu},
    {0x98d2d2c78fdda5bau, 0x1c4c8b1349b9b562u}, {0x9f83c3bcb9ea8794u, 0x11afd6ec0e14115du},
    {0x0764b4abe8652979u, 0x161bcca7119915b5u}, {0x493de1d6e27e73d7u, 0x1ba2bfd0d5ff5b22u},
    {0x6dc6ad264d8f0866u, 0x1145b7e285bf98f5u}, {0xc938586fe0f2ca80u, 0x159725db272f7f32u},
    {0x7b866e8bd92f7d20u, 0x1afcef51f0fb5effu}, {0xad34051767bdae34u, 0x10de1593369d1b5fu},
    {0x9881065d41ad19c1u, 0x15159af804446237u}, {0x7ea147f492186032u, 0x1a5b01b605557ac5u},
    {0x6f24ccf8db4f3c1fu, 0x1078e111c3556cbbu}, {0x4aee003712230b27u, 0x14971956342ac7eau},
    {0xdda98044d6abcdf0u, 0x19bcdfabc13579e4u}, {0x0a89f02b062b60b6u, 0x10160bcb58c16c2fu},
    {0xcd2c6c35c7b638e4u, 0x141b8ebe2ef1c73au}, {0x8077874339a3c71du, 0x1922726dbaae3909u},
    {0xe0956914080cb8e4u, 0x1f6b0f092959c74bu}, {0x6c5d61ac8507f38eu, 0x13a2e965b9d81c8fu},
    {0x4774ba17a649f072u, 0x188ba3bf284e23b3u}, {0x1951e89d8fdc6c8fu, 0x1eae8caef261aca0u},
    {0x0fd3316279e9c3d9u, 0x132d17ed577d0be4u}, {0x13c7fdbb186434cfu, 0x17f85de8ad5c4eddu},
    {0x58b9fd29de7d4203u, 0x1df67562d8b36294u}, {0xb7743e3a2b0e4942u, 0x12ba095dc7701d9cu},
    {0xe5514dc8b5d1db92u, 0x17688bb5394c2503u}, {0xdea5a13ae3465277u, 0x1d42aea2879f2e44u},
    {0x0b2784c4ce0bf38au, 0x1249ad2594c37cebu}, {0xcdf165f6018ef06du, 0x16dc186ef9f45c25u},
    {0x416dbf7381f2ac88u, 0x1c931e8ab871732fu}, {0x88e497a83137abd5u, 0x11dbf316b346e7fdu},
    {0xeb1dbd923d8596cau, 0x1652efdc6018a1fcu}, {0x25e52cf6cce6fc7du, 0x1be7abd3781eca7cu},
    {0x97af3c1a40105dceu, 0x1170cb642b133e8du}, {0xfd9b0b20d0147542u, 0x15ccfe3d35d80e30u},
    {0x3d01cde904199292u, 0x1b403dcc834e11bdu}, {0x462120b1a28ffb9bu, 0x1108269fd210cb16u},
    {0xd7a968de0b33fa82u, 0x154a3047c694fddbu}, {0xcd93c3158e00f923u, 0x1a9cbc59b83a3d52u},
    {0xc07c59ed78c09bb6u, 0x10a1f5b813246653u}, {0xb09b7068d6f0c2a3u, 0x14ca732617ed7fe8u},
    {0xdcc24c830cacf34cu, 0x19fd0fef9de8dfe2u}, {0xc9f96fd1e7ec180fu, 0x103e29f5c2b18bedu},
    {0x3c77cbc661e71e13u, 0x144db473335deee9u}, {0x8b95beb7fa60e598u, 0x1961219000356aa3u},
    {0x6e7b2e65f8f91efeu, 0x1fb969f40042c54cu}, {0xc50cfcffbb9bb35fu, 0x13d3e2388029bb4fu},
    {0xb6503c3faa82a037u, 0x18c8dac6a0342a23u}, {0xa3e44b4f95234844u, 0x1efb1178484134acu},
    {0xe66eaf11bd360d2bu, 0x135ceaeb2d28c0ebu}, {0xe00a5ad62c839075u, 0x183425a5f872f126u},
    {0x980cf18bb7a47493u, 0x1e412f0f768fad70u}, {0x5f0816f752c6c8dcu, 0x12e8bd69aa19cc66u},
    {0xf6ca1cb527787b13u, 0x17a2ecc414a03f7fu}, {0xf47ca3e2715699d7u, 0x1d8ba7f519c84f5fu},
    {0xf8cde66d86d62026u, 0x127748f9301d319bu}, {0xf7016008e88ba830u, 0x17151b377c247e02u},
    {0xb4c1b80b22ae923cu, 0x1cda62055b2d9d83u}, {0x50f91306f5ad1b65u, 0x12087d4358fc8272u},
    {0xe53757c8b318623fu, 0x168a9c942f3ba30eu}, {0x9e852dbadfde7acfu, 0x1c2d43b93b0a8bd2u},
    {0xa3133c94cbeb0cc1u, 0x119c4a53c4e69763u}, {0x8bd80bb9fee5cff1u, 0x16035ce8b6203d3cu},
    {0xaece0ea87e9f43eeu, 0x1b843422e3a84c8bu}, {0x4d40c9294f238a75u, 0x1132a095ce492fd7u},
    {0x2090fb73a2ec6d12u, 0x157f48bb41db7bcdu}, {0x68b53a508ba78856u, 0x1adf1aea12525ac0u},
    {0x417144725748b536u, 0x10cb70d24b7378b8u}, {0x51cd958eed1ae283u, 0x14fe4d06de5056e6u},
    {0xe640faf2a8619b24u, 0x1a3de04895e46c9fu}, {0xefe89cd7a93d00f7u, 0x1066ac2d5daec3e3u},
    {0xebe2c40d938c4134u, 0x14805738b51a74dcu}, {0x26db7510f86f5181u, 0x19a06d06e2611214u},
    {0x9849292a9b4592f1u, 0x100444244d7cab4cu}, {0xbe5b73754216f7adu, 0x1405552d60dbd61fu},
    {0xadf25052929cb598u, 0x1906aa78b912cba7u}, {0x996ee4673743e2ffu, 0x1f485516e7577e91u},
    {0xffe54ec0828a6ddfu, 0x138d352e5096af1au}, {0xbfdea270a32d0957u, 0x18708279e4bc5ae1u},
    {0x2fd64b0ccbf84badu, 0x1e8ca3185deb719au}, {0x5de5eee7ff7b2f4cu, 0x1317e5ef3ab32700u},
    {0x755f6aa1ff59fb1fu, 0x17dddf6b095ff0c0u}, {0x92b7454a7f3079e7u, 0x1dd55745cbb7ecf0u},
    {0x5bb28b4e8f7e4c30u, 0x12a5568b9f52f416u}, {0xf29f2e22335ddf3cu, 0x174eac2e8727b11bu},
    {0xef46f9aac035570bu, 0x1d22573a28f19d62u}, {0xd58c5c0ab8215667u, 0x123576845997025du},
    {0x4aef730d6629ac01u, 0x16c2d4256ffcc2f5u}, {0x9dab4fd0bfb41701u, 0x1c73892ecbfbf3b2u},
    {0xa28b11e277d08e60u, 0x11c835bd3f7d784fu}, {0x8b2dd65b15c4b1f9u, 0x163a432c8f5cd663u},
    {0x6df94bf1db35de77u, 0x1bc8d3f7b3340bfcu}, {0xc4bbcf772901ab0au, 0x115d847ad000877du},
    {0x35eac354f34215cdu, 0x15b4e5998400a95du}, {0x8365742a30129b40u, 0x1b221effe500d3b4u},
    {0xd21f689a5e0ba108u, 0x10f5535fef208450u}, {0x06a742c0f58e894au, 0x1532a837eae8a565u},
    {0x4851137132f22b9du, 0x1a7f5245e5a2cebeu}, {0xed32ac26bfd75b42u, 0x108f936baf85c136u},
    {0xa87f57306fcd3212u, 0x14b378469b673184u}, {0xd29f2cfc8bc07e97u, 0x19e056584240fde5u},
    {0xa3a37c1dd7584f1eu, 0x102c35f729689eafu}, {0x8c8c5b254d2e62e6u, 0x14374374f3c2c65bu},
    {0x6faf71eea079fb9fu, 0x1945145230b377f2u}, {0x0b9b4e6a48987a87u, 0x1f965966bce055efu},
    {0x674111026d5f4c94u, 0x13bdf7e0360c35b5u}, {0xc111554308b71fbau, 0x18ad75d8438f4322u},
    {0x7155aa93cae4e7a8u, 0x1ed8d34e547313ebu}, {0x26d58a9c5ecf10c9u, 0x13478410f4c7ec73u},
    {0xf08aed437682d4fbu, 0x1819651531f9e78fu}, {0xecada89454238a3au, 0x1e1fbe5a7e786173u},
    {0x73ec895cb4963664u, 0x12d3d6f88f0b3ce8u}, {0x90e7abb3e1bbc3fdu, 0x1788ccb6b2ce0c22u},
    {0x352196a0da2ab4fdu, 0x1d6affe45f818f2bu}, {0x0134fe24885ab11eu, 0x1262dfeebbb0f97bu},
    {0xc1823dadaa715d65u, 0x16fb97ea6a9d37d9u}, {0x31e2cd19150db4bfu, 0x1cba7de5054485d0u},
    {0x1f2dc02fad2890f7u, 0x11f48eaf234ad3a2u}, {0xa6f9303b9872b535u, 0x1671b25aec1d888au},
    {0x50b77c4a7e8f6282u, 0x1c0e1ef1a724eaadu}, {0x5272adae8f199d91u, 0x1188d357087712acu},
    {0x670f591a32e004f6u, 0x15eb082cca94d757u}, {0x40d32f60bf980633u, 0x1b65ca37fd3a0d2du},
    {0x4883fd9c77bf03e0u, 0x111f9e62fe44483cu}, {0x5aa4fd0395aec4d8u, 0x156785fbbdd55a4bu},
    {0x314e3c447b1a760eu, 0x1ac1677aad4ab0deu}, {0xded0e5aaccf089c9u, 0x10b8e0acac4eae8au},
    {0x96851f15802cac3bu, 0x14e718d7d7625a2du}, {0xfc2666dae037d74au, 0x1a20df0dcd3af0b8u},
    {0x9d980048cc22e68eu, 0x10548b68a044d673u}, {0x84fe005aff2ba032u, 0x1469ae42c8560c10u},
    {0xa63d8071bef6883eu, 0x198419d37a6b8f14u}, {0xcfcce08e2eb42a4eu, 0x1fe52048590672d9u},
    {0x21e00c58dd309a70u, 0x13ef342d37a407c8u}, {0x2a580f6f147cc10du, 0x18eb0138858d09bau},
    {0xb4ee134ad99bf150u, 0x1f25c186a6f04c28u}, {0x7114cc0ec80176d2u, 0x137798f428562f99u},
    {0xcd59ff127a01d486u, 0x18557f31326bbb7fu}, {0xc0b07ed7188249a8u, 0x1e6adefd7f06aa5fu},
    {0xd86e4f466f516e09u, 0x1302cb5e6f642a7bu}, {0xce89e3180b25c98bu, 0x17c37e360b3d351au},
    {0x822c5bde0def3beeu, 0x1db45dc38e0c8261u}, {0xf15bb96ac8b58575u, 0x1290ba9a38c7d17cu},
    {0x2db2a7c57ae2e6d2u, 0x1734e940c6f9c5dcu}, {0x391f51b6d99ba086u, 0x1d022390f8b83753u},
    {0x03b3931248014454u, 0x1221563a9b732294u}, {0x04a077d6da019569u, 0x16a9abc9424feb39u},
    {0x45c895cc9081fac3u, 0x1c5416bb92e3e607u}, {0x8b9d5d9fda513cbau, 0x11b48e353bce6fc4u},
    {0xae84b507d0e58be8u, 0x1621b1c28ac20bb5u}, {0x1a25e249c51eeee3u, 0x1baa1e332d728ea3u},
    {0xf057ad6e1b33554du, 0x114a52dffc679925u}, {0x6c6d98c9a2002aa1u, 0x159ce797fb817f6fu},
    {0x4788fefc0a803549u, 0x1b04217dfa61df4bu}, {0x0cb59f5d8690214eu, 0x10e294eebc7d2b8fu},
    {0xcfe30734e83429a1u, 0x151b3a2a6b9c7672u}, {0x83dbc9022241340au, 0x1a6208b50683940fu},
    {0xb2695da15568c086u, 0x107d457124123c89u}, {0x1f03b509aac2f0a7u, 0x149c96cd6d16cbacu},
    {0x26c4a24c1573acd1u, 0x19c3bc80c85c7e97u}, {0x783ae56f8d684c03u, 0x101a55d07d39cf1eu},
    {0x16499ecb70c25f03u, 0x1420eb449c8842e6u}, {0x9bdc067e4cf2f6c4u, 0x19292615c3aa539fu},
    {0x82d3081de02fb476u, 0x1f736f9b3494e887u}, {0xb1c3e512ac1dd0c9u, 0x13a825c100dd1154u},
    {0xde34de57572544fcu, 0x18922f31411455a9u}, {0x55c215ed2cee963bu, 0x1eb6bafd91596b14u},
    {0xb5994db43c151de5u, 0x133234de7ad7e2ecu}, {0xe2ffa1214b1a655eu, 0x17fec216198ddba7u},
    {0xdbbf89699de0feb6u, 0x1dfe729b9ff15291u}, {0x2957b5e202ac9f31u, 0x12bf07a143f6d39bu},
    {0xf3ada35a8357c6feu, 0x176ec98994f48881u}, {0x70990c31242db8bdu, 0x1d4a7bebfa31aaa2u},
    {0x865fa79eb69c9376u, 0x124e8d737c5f0aa5u}, {0xe7f791866443b854u, 0x16e230d05b76cd4eu},
    {0xa1f575e7fd54a669u, 0x1c9abd04725480a2u}, {0xa53969b0fe54e801u, 0x11e0b622c774d065u},
    {0x0e87c41d3dea2202u, 0x1658e3ab7952047fu}, {0xd229b5248d64aa82u, 0x1bef1c9657a6859eu},
    {0x435a1136d85eea91u, 0x117571ddf6c81383u}, {0x143095848e76a536u, 0x15d2ce55747a1864u},
    {0x193cbae5b2144e83u, 0x1b4781ead1989e7du}, {0x2fc5f4cf8f4cb112u, 0x110cb132c2ff630eu},
    {0xbbb77203731fdd56u, 0x154fdd7f73bf3bd1u}, {0x2aa54e844fe7d4acu, 0x1aa3d4df50af0ac6u},
    {0xdaa75112b1f0e4ebu, 0x10a6650b926d66bbu}, {0xd15125575e6d1e26u, 0x14cffe4e7708c06au},
    {0x85a56ead360865b0u, 0x1a03fde214caf085u}, {0x7387652c41c53f8eu, 0x10427ead4cfed653u},
    {0x50693e7752368f71u, 0x14531e58a03e8be8u}, {0x64838e1526c4334eu, 0x1967e5eec84e2ee2u},
    {0xfda4719a70754022u, 0x1fc1df6a7a61ba9au}, {0xde86c70086494815u, 0x13d92ba28c7d14a0u},
    {0x162878c0a7db9a1au, 0x18cf768b2f9c59c9u}, {0x5bb296f0d1d280a1u, 0x1f03542dfb83703bu},
    {0x194f9e5683239064u, 0x1362149cbd322625u}, {0x5fa385ec23ec747eu, 0x183a99c3ec7eafaeu},
    {0xf78c67672ce7919du, 0x1e494034e79e5b99u}, {0x3ab7c0a07c10bb02u, 0x12edc82110c2f940u},
    {0x4965b0c89b14e9c3u, 0x17a93a2954f3b790u}, {0x5bbf1cfac1da2433u, 0x1d9388b3aa30a574u},
    {0xb957721cb92856a0u, 0x127c35704a5e6768u}, {0xe7ad4ea3e7726c48u, 0x171b42cc5cf60142u},
    {0xa198a24ce14f075au, 0x1ce2137f74338193u}, {0x44ff65700cd16498u, 0x120d4c2fa8a030fcu},
    {0x563f3ecc1005bdbeu, 0x16909f3b92c83d3bu}, {0x2bcf0e7f14072d2eu, 0x1c34c70a777a4c8au},
    {0x5b61690f6c847c3du, 0x11a0fc668aac6fd6u}, {0xf239c35347a59b4cu, 0x16093b802d578bcbu},
    {0xeec83428198f021fu, 0x1b8b8a6038ad6ebeu}, {0x553d20990ff96153u, 0x1137367c236c6537u},
    {0x2a8c68bf53f7b9a8u, 0x1585041b2c477e85u}, {0x752f82ef28f5a812u, 0x1ae64521f7595e26u},
    {0x093db1d57999890bu, 0x10cfeb353a97dad8u}, {0x0b8d1e4ad7ffeb4eu, 0x1503e602893dd18eu},
    {0x8e7065dd8dffe622u, 0x1a44df832b8d45f1u}, {0xf9063faa78bfefd5u, 0x106b0bb1fb384bb6u},
    {0xb747cf9516efebcau, 0x1485ce9e7a065ea4u}, {0xe519c37a5cabe6bdu, 0x19a742461887f64du},
    {0xaf301a2c79eb7036u, 0x1008896bcf54f9f0u}, {0xdafc20b798664c43u, 0x140aabc6c32a386cu},
    {0x11bb28e57e7fdf54u, 0x190d56b873f4c688u}, {0x1629f31ede1fd72au, 0x1f50ac6690f1f82au},
    {0x4dda37f34ad3e67au, 0x13926bc01a973b1au}, {0xe150c5f01d88e019u, 0x187706b0213d09e0u},
    {0x19a4f76c24eb181fu, 0x1e94c85c298c4c59u}, {0xb0071aa39712ef13u, 0x131cfd3999f7afb7u},
    {0x9c08e14c7cd7aad8u, 0x17e43c8800759ba5u}, {0x030b199f9c0d958eu, 0x1ddd4baa0093028fu},
    {0x61e6f003c1887d79u, 0x12aa4f4a405be199u}, {0xba60ac04b1ea9cd7u, 0x1754e31cd072d9ffu},
    {0xa8f8d705de65440du, 0x1d2a1be4048f907fu}, {0xc99b8663aaff4a88u, 0x123a516e82d9ba4fu},
    {0xbc0267fc95bf1d2au, 0x16c8e5ca239028e3u}, {0xab0301fbbb2ee474u, 0x1c7b1f3cac74331cu},
    {0xeae1e13d54fd4ec9u, 0x11ccf385ebc89ff1u}, {0x659a598caa3ca27bu, 0x1640306766bac7eeu},
    {0xff00efefd4cbcb1au, 0x1bd03c81406979e9u}, {0x3f6095f5e4ff5ef0u, 0x116225d0c841ec32u},
    {0xcf38bb735e3f36acu, 0x15baaf44fa52673eu}, {0x8306ea5035cf0457u, 0x1b295b1638e7010eu},
    {0x11e4527221a162b6u, 0x10f9d8ede39060a9u}, {0x565d670eaa09bb64u, 0x15384f295c7478d3u},
    {0x2bf4c0d2548c2a3du, 0x1a8662f3b3919708u}, {0x1b78f88374d79a66u, 0x1093fdd8503afe65u},
    {0x625736a4520d8100u, 0x14b8fd4e6449bdfeu}, {0xfaed044d6690e140u, 0x19e73ca1fd5c2d7du},
    {0xbcd422b0601a8cc8u, 0x103085e53e599c6eu}, {0x6c092b5c78212ffau, 0x143ca75e8df0038au},
    {0x070b763396297bf8u, 0x194bd136316c046du}, {0x48ce53c07bb3daf6u, 0x1f9ec583bdc70588u},
    {0x2d80f4584d5068dau, 0x13c33b72569c6375u}, {0x78e1316e60a48310u, 0x18b40a4eec437c52u},
};

static inline uint32_t ryuPow5Bits(int32_t e) { return (uint32_t)(((uint32_t)e * 1217359) >> 19) + 1; }
static inline uint32_t ryuLog10Pow2(int32_t e) { return ((uint32_t)e * 78913) >> 18; }
static inline uint32_t ryuLog10Pow5(int32_t e) { return ((uint32_t)e * 732923) >> 20; }

static inline bool ryuMultipleOfPow5(uint64_t v, uint32_t p) {
    uint32_t n = 0;
    while (v % 5 == 0) {
        v /= 5;
        n++;
    }
    return n >= p;
}

__extension__ typedef unsigned __int128 ryuU128;

static inline uint64_t ryuMulShift(uint64_t m, const uint64_t *mul, int32_t j) {
    ryuU128 b0 = (ryuU128)m * mul[0];
    ryuU128 b2 = (ryuU128)m * mul[1];
    return (uint64_t)(((b0 >> 64) + b2) >> (j - 64));
}

/* The digits of a finite, nonzero float as a number, and in *exp10 the
   power of ten of its last digit. mant and ieeeExp are the float's fields;
   mbits and bias describe its format (52 and 1023 for f64). */
static uint64_t ryuDigits(uint64_t mant, uint32_t ieeeExp, int mbits, int bias, int32_t *exp10) {
    int32_t e2;
    uint64_t m2;
    if (ieeeExp == 0) {
        e2 = 1 - bias - mbits - 2;
        m2 = mant;
    } else {
        e2 = (int32_t)ieeeExp - bias - mbits - 2;
        m2 = (1ull << mbits) | mant;
    }
    bool even = (m2 & 1) == 0;
    uint64_t mv = 4 * m2;
    uint32_t mmShift = mant != 0 || ieeeExp <= 1;
    uint64_t vr, vp, vm;
    int32_t e10;
    bool vmTrailingZeros = false, vrTrailingZeros = false;
    if (e2 >= 0) {
        uint32_t q = ryuLog10Pow2(e2) - (e2 > 3);
        e10 = (int32_t)q;
        int32_t k = 125 + (int32_t)ryuPow5Bits((int32_t)q) - 1;
        int32_t i = -e2 + (int32_t)q + k;
        vr = ryuMulShift(4 * m2, ryuPow5Inv[q], i);
        vp = ryuMulShift(4 * m2 + 2, ryuPow5Inv[q], i);
        vm = ryuMulShift(4 * m2 - 1 - mmShift, ryuPow5Inv[q], i);
        if (q <= 21) {
            if (mv % 5 == 0) {
                vrTrailingZeros = ryuMultipleOfPow5(mv, q);
            } else if (even) {
                vmTrailingZeros = ryuMultipleOfPow5(mv - 1 - mmShift, q);
            } else {
                vp -= ryuMultipleOfPow5(mv + 2, q);
            }
        }
    } else {
        uint32_t q = ryuLog10Pow5(-e2) - (-e2 > 1);
        e10 = (int32_t)q + e2;
        int32_t i = -e2 - (int32_t)q;
        int32_t k = (int32_t)ryuPow5Bits(i) - 125;
        int32_t j = (int32_t)q - k;
        vr = ryuMulShift(4 * m2, ryuPow5[i], j);
        vp = ryuMulShift(4 * m2 + 2, ryuPow5[i], j);
        vm = ryuMulShift(4 * m2 - 1 - mmShift, ryuPow5[i], j);
        if (q <= 1) {
            vrTrailingZeros = true;
            if (even) {
                vmTrailingZeros = mmShift == 1;
            } else {
                vp--;
            }
        } else if (q < 63) {
            vrTrailingZeros = (mv & ((1ull << q) - 1)) == 0;
        }
    }
    /* drop digits while the bounds still differ */
    int32_t removed = 0;
    uint8_t last = 0;
    uint64_t out;
    if (vmTrailingZeros || vrTrailingZeros) {
        while (vp / 10 > vm / 10) {
            vmTrailingZeros &= vm % 10 == 0;
            vrTrailingZeros &= last == 0;
            last = (uint8_t)(vr % 10);
            vr /= 10;
            vp /= 10;
            vm /= 10;
            removed++;
        }
        if (vmTrailingZeros) {
            while (vm % 10 == 0) {
                vrTrailingZeros &= last == 0;
                last = (uint8_t)(vr % 10);
                vr /= 10;
                vp /= 10;
                vm /= 10;
                removed++;
            }
        }
        if (vrTrailingZeros && last == 5 && vr % 2 == 0) last = 4;   /* a tie goes to even */
        out = vr + ((vr == vm && (!even || !vmTrailingZeros)) || last >= 5);
    } else {
        bool roundUp = false;
        if (vp / 100 > vm / 100) {
            roundUp = vr % 100 >= 50;
            vr /= 100;
            vp /= 100;
            vm /= 100;
            removed += 2;
        }
        while (vp / 10 > vm / 10) {
            roundUp = vr % 10 >= 5;
            vr /= 10;
            vp /= 10;
            vm /= 10;
            removed++;
        }
        out = vr + (vr == vm || roundUp);
    }
    *exp10 = e10 + removed;
    return out;
}

#endif
