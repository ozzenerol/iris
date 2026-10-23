## Numbers of any size

`big.parse` reads a whole number from text and `big.from` makes one from an `i64`. Each operation gives a new `Int`, and `{n}` writes it in decimal:

```iris run
import big

!void main() {
    scoped a := try big.parse("123456789012345678901234567890")
    scoped b := big.from(-987654321)
    scoped sum := a.add(b)
    scoped product := a.mul(b)
    q, r := a.divMod(b)
    defer {
        q.free()
        r.free()
    }
    print("{sum}\n{product}\n{q} remainder {r}")
}
```

```output
123456789012345678900246913569
-121932631124828532112482853211126352690
-124999998873437499901 remainder 574845669
```

Division rounds toward zero and the remainder has the sign of the number divided, as with integers.

## Powers and other bases

`pow` raises to a power, `modPow` to a power modulo a number without making the whole power, and `text` writes a number in any base from 2 to 36:

```iris run
import big

!void main() {
    scoped two := big.from(2)
    scoped p := two.pow(100)
    scoped hex := p.text(16)
    print("{p} has {p.bitLen()} bits: {hex}")
    scoped m := try big.parse("1000000007")
    scoped exp := try big.parse("1000000005")
    scoped inv := big.from(3).modPow(exp, m)
    print("3 * {inv} mod {m} is 1")
    scoped mask := try big.parse("ffff_ffff_ffff_ffff_ffff", 16)
    n := mask.toI64() catch e {
        print("{e.code}")
        0
    }
}
```

```output
1267650600228229401496703205376 has 101 bits: 10000000000000000000000000
3 * 333333336 mod 1000000007 is 1
BigError.tooBig
```
