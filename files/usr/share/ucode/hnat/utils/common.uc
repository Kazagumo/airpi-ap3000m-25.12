'use strict';

export function log() {
    return {
        info: (msg) => system(sprintf('logger -t hnat-detect "%s"', msg)),
        warn: (msg) => system(sprintf('logger -p user.warn -t hnat-detect "%s"', msg)),
        err:  (msg) => system(sprintf('logger -p user.err -t hnat-detect "%s"', msg))
    };
}

export function merge(dst, src) {
    if (!dst)
        dst = {};
    if (!src)
        return dst;

    for (let k in src)
        dst[k] = src[k];

    return dst;
}

export function read_pipe(cmd) {
    let p = popen(cmd);
    if (!p)
        return "";

    let out = trim(p.read("all") || "");
    p.close();
    return out;
}
