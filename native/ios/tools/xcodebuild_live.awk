/^[ \t]/ || /^$/ { next }
/(error|warning): / || /^\*\* / {
    if ($0 in shown) next
    shown[$0] = 1
    print substr($0, 1, 300)
    fflush()
    next
}
/ \(in target '/ {
    target = $0
    sub(/.* \(in target '/, "", target)
    sub(/'.*/, "", target)
    file = ""
    for (i = 2; i <= NF; i++) {
        if ($i == "(in") break
        if ($i ~ /^\//) file = $i
    }
    sub(/.*\//, "", file)
    print $1 (file == "" ? "" : " " file) " [" target "]"
    fflush()
    next
}
/\/DerivedData\// {
    line = $1
    for (i = 2; i <= NF; i++) {
        field = $i
        if (field ~ /^\/.*\/DerivedData\//) sub(/.*\//, "", field)
        line = line " " field
    }
    print substr(line, 1, 200)
    fflush()
    next
}
{
    print substr($0, 1, 200)
    fflush()
}
