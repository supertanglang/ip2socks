# ip2socks

## Get start

```bash
git submodule init
git submodule update
git submodule foreach git pull

# --recursive

# archlinux
vagrant up --provider virtualbox
vagrant ssh
```

* `use-vc` in `/etc/resolv.conf`: Sets RES_USEVC in _res.options.  This option forces the use of TCP for DNS resolutions.

## Library

* [antirez/sds](https://github.com/antirez/sds)

## Remove submodule

```
Run git rm --cached <submodule name>
Delete the relevant lines from the .gitmodules file.
Delete the relevant section from .git/config.
Commit
Delete the now untracked submodule files.
Remove directory .git/modules/<submodule name>
```

## References

* [ANDROID: BADVPN中使用的LWIP的版本及TUN2SOCKS工作原理](https://www.brobwind.com/archives/1401)
* [takayuki/lwip-tap](https://github.com/takayuki/lwip-tap)
* [tun2tor](https://github.com/iCepa/tun2tor)
* [FlowerWrong/ShadowVPN](https://github.com/FlowerWrong/ShadowVPN)

## dns

* [DNSPod/dnspod-sr](https://github.com/DNSPod/dnspod-sr)
* [vstakhov/librdns](https://github.com/vstakhov/librdns)
* [jtripper/dns-tcp-socks-proxy](https://github.com/jtripper/dns-tcp-socks-proxy)
* [vietor/dnsproxy](https://github.com/vietor/dnsproxy)
* [getdnsapi/getdns](https://github.com/getdnsapi/getdns)
* [NLnetLabs/ldns](https://github.com/NLnetLabs/ldns)
