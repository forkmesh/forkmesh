package com.forkmesh.nativeapp.nexus

class PortParseException(message: String) : IllegalArgumentException(message)

class PortList(ports: Iterable<Int>) {
    val ports: List<Int> = ports.filter { it in 1..65535 }.toSortedSet().toList()

    val count: Int get() = ports.size
    val isEmpty: Boolean get() = ports.isEmpty()

    override fun toString(): String {
        val parts = mutableListOf<String>()
        var index = 0
        while (index < ports.size) {
            var end = index
            while (end + 1 < ports.size && ports[end + 1] == ports[end] + 1) end++
            parts += if (end - index >= 2) "${ports[index]}-${ports[end]}" else (index..end).joinToString(",") { ports[it].toString() }
            index = end + 1
        }
        return parts.joinToString(",")
    }

    fun union(other: PortList): PortList = PortList(ports + other.ports)

    override fun equals(other: Any?): Boolean = other is PortList && ports == other.ports
    override fun hashCode(): Int = ports.hashCode()

    companion object {
        fun parse(text: String): PortList {
            val tokens = text.split(',', ' ', '\t', '\n').filter { it.isNotEmpty() }
            if (tokens.isEmpty()) throw PortParseException("Enter at least one port.")
            val set = sortedSetOf<Int>()
            for (token in tokens) {
                val parts = token.split("-")
                if (parts.size > 2 || parts.any { part -> part.isEmpty() || part.length > 5 || !part.all { it in '0'..'9' } }) {
                    throw PortParseException("“$token” is not a port or port range.")
                }
                val numbers = parts.map { it.toInt() }
                if (numbers.any { it !in 1..65535 }) throw PortParseException("“$token” is outside 1–65535.")
                if (numbers.size == 2) {
                    if (numbers[0] > numbers[1]) throw PortParseException("The range “$token” ends before it starts.")
                    set.addAll(numbers[0]..numbers[1])
                } else {
                    set += numbers[0]
                }
            }
            return PortList(set)
        }

        fun decode(text: String): PortList = if (text.isEmpty()) PortList(emptyList()) else runCatching { parse(text) }.getOrDefault(PortList(emptyList()))
    }
}

enum class PortPreset(val raw: String, val title: String, val icon: String) {
    Quick("quick", "Quick (20)", "bolt"),
    Common("common", "Common (100)", "list"),
    Web("web", "Web", "language"),
    RemoteAccess("remoteAccess", "Remote access", "terminal"),
    FileSharing("fileSharing", "File sharing", "folder"),
    Mail("mail", "Mail", "mail"),
    SmartHome("smartHome", "Smart home & IoT", "home"),
    Media("media", "Media & casting", "tv"),
    Databases("databases", "Databases", "storage"),
    WellKnown("wellKnown", "Well-known (1–1024)", "tag"),
    All("all", "All ports (1–65535)", "apps");

    val ports: PortList
        get() = when (this) {
            Quick -> PortList(QUICK)
            Common -> PortList(COMMON)
            Web -> PortList(listOf(80, 81, 443, 591, 3000, 4443, 5000, 5001, 7080, 7443, 8000, 8008, 8080, 8081, 8088, 8443, 8888, 9000, 9090, 9443))
            RemoteAccess -> PortList(listOf(22, 23, 2222, 3389, 5900, 5901, 5938, 6568, 7070, 8291, 5985, 5986))
            FileSharing -> PortList(listOf(20, 21, 69, 111, 135, 137, 139, 445, 548, 873, 2049, 5005, 8384, 22000))
            Mail -> PortList(listOf(25, 110, 143, 465, 587, 993, 995, 4190))
            SmartHome -> PortList(listOf(80, 443, 554, 1400, 1883, 1900, 5353, 5540, 6053, 6466, 6467, 8008, 8009, 8060, 8123, 8883, 9000, 9123, 21063, 40000, 49152, 55443))
            Media -> PortList(listOf(554, 1400, 3689, 5000, 7000, 7100, 8008, 8009, 8060, 8096, 8200, 8443, 9080, 32400, 49153))
            Databases -> PortList(listOf(1433, 1521, 3306, 5432, 5984, 6379, 7474, 8086, 9042, 9200, 11211, 27017, 28015))
            WellKnown -> PortList(1..1024)
            All -> PortList(1..65535)
        }

    companion object {
        val QUICK = listOf(21, 22, 23, 25, 53, 80, 110, 139, 143, 443, 445, 554, 3306, 3389, 5900, 8000, 8080, 8443, 9100, 62078)

        val COMMON = listOf(
            7, 9, 13, 21, 22, 23, 25, 26, 37, 53, 79, 80, 81, 88, 106, 110, 111, 113, 119, 135,
            139, 143, 144, 179, 199, 389, 427, 443, 444, 445, 465, 513, 514, 515, 543, 544, 548, 554, 587, 631,
            646, 873, 990, 993, 995, 1025, 1026, 1027, 1028, 1029, 1110, 1433, 1720, 1723, 1755, 1883, 1900, 2000, 2001, 2049,
            2121, 2717, 3000, 3128, 3306, 3389, 3986, 4899, 5000, 5009, 5051, 5060, 5101, 5190, 5357, 5432, 5631, 5666, 5800, 5900,
            6000, 6001, 6646, 7070, 8000, 8008, 8009, 8080, 8081, 8443, 8888, 9100, 9999, 10000, 32768, 49152, 49153, 49154, 49155, 62078,
        )

        val scanChoices = listOf(Quick, Common, Web, RemoteAccess, FileSharing)
    }
}

data class KnownService(val port: Int, val name: String, val title: String, val cleartext: Boolean = false, val sensitive: Boolean = false)

object ServiceCatalog {
    fun service(port: Int): KnownService? = table[port]

    fun name(port: Int): String? = table[port]?.name

    val all: List<KnownService> = listOf(
        KnownService(7, "echo", "Echo", cleartext = true),
        KnownService(9, "discard", "Discard"),
        KnownService(13, "daytime", "Daytime"),
        KnownService(20, "ftp-data", "FTP data", cleartext = true),
        KnownService(21, "ftp", "FTP", cleartext = true, sensitive = true),
        KnownService(22, "ssh", "SSH", sensitive = true),
        KnownService(23, "telnet", "Telnet", cleartext = true, sensitive = true),
        KnownService(25, "smtp", "SMTP", cleartext = true),
        KnownService(26, "smtp-alt", "SMTP (alternate)", cleartext = true),
        KnownService(37, "time", "Time"),
        KnownService(53, "domain", "DNS"),
        KnownService(69, "tftp", "TFTP", cleartext = true, sensitive = true),
        KnownService(79, "finger", "Finger", cleartext = true),
        KnownService(80, "http", "HTTP", cleartext = true),
        KnownService(81, "http-alt", "HTTP (alternate)", cleartext = true),
        KnownService(88, "kerberos", "Kerberos"),
        KnownService(106, "pop3pw", "POP3 password", cleartext = true),
        KnownService(110, "pop3", "POP3", cleartext = true),
        KnownService(111, "rpcbind", "RPC portmapper", sensitive = true),
        KnownService(113, "ident", "Ident"),
        KnownService(119, "nntp", "NNTP", cleartext = true),
        KnownService(123, "ntp", "NTP"),
        KnownService(135, "msrpc", "Microsoft RPC", sensitive = true),
        KnownService(137, "netbios-ns", "NetBIOS name"),
        KnownService(139, "netbios-ssn", "NetBIOS session", sensitive = true),
        KnownService(143, "imap", "IMAP", cleartext = true),
        KnownService(144, "news", "NeWS"),
        KnownService(179, "bgp", "BGP"),
        KnownService(199, "smux", "SNMP multiplexer"),
        KnownService(389, "ldap", "LDAP", cleartext = true, sensitive = true),
        KnownService(427, "svrloc", "Service Location Protocol"),
        KnownService(443, "https", "HTTPS"),
        KnownService(444, "snpp", "SNPP"),
        KnownService(445, "microsoft-ds", "SMB", sensitive = true),
        KnownService(465, "smtps", "SMTP over TLS"),
        KnownService(513, "login", "rlogin", cleartext = true, sensitive = true),
        KnownService(514, "shell", "rsh", cleartext = true, sensitive = true),
        KnownService(515, "printer", "LPD printing"),
        KnownService(543, "klogin", "Kerberos login"),
        KnownService(544, "kshell", "Kerberos shell"),
        KnownService(548, "afp", "Apple Filing Protocol", sensitive = true),
        KnownService(554, "rtsp", "RTSP streaming", cleartext = true),
        KnownService(587, "submission", "Mail submission"),
        KnownService(591, "http-alt", "HTTP (FileMaker)", cleartext = true),
        KnownService(631, "ipp", "Internet Printing Protocol"),
        KnownService(646, "ldp", "LDP"),
        KnownService(873, "rsync", "rsync", cleartext = true, sensitive = true),
        KnownService(990, "ftps", "FTP over TLS"),
        KnownService(993, "imaps", "IMAP over TLS"),
        KnownService(995, "pop3s", "POP3 over TLS"),
        KnownService(1025, "nfs-or-iis", "Dynamic RPC"),
        KnownService(1110, "nfsd-status", "NFS status"),
        KnownService(1400, "sonos", "Sonos control", cleartext = true),
        KnownService(1433, "ms-sql-s", "Microsoft SQL Server", sensitive = true),
        KnownService(1521, "oracle", "Oracle database", sensitive = true),
        KnownService(1720, "h323q931", "H.323"),
        KnownService(1723, "pptp", "PPTP VPN", sensitive = true),
        KnownService(1755, "wms", "Windows Media"),
        KnownService(1883, "mqtt", "MQTT", cleartext = true, sensitive = true),
        KnownService(1900, "upnp", "UPnP / SSDP"),
        KnownService(2000, "cisco-sccp", "Cisco SCCP"),
        KnownService(2049, "nfs", "NFS", sensitive = true),
        KnownService(2121, "ccproxy-ftp", "FTP (alternate)", cleartext = true),
        KnownService(2222, "ssh-alt", "SSH (alternate)", sensitive = true),
        KnownService(3000, "http-dev", "Web app (development)", cleartext = true),
        KnownService(3128, "squid-http", "HTTP proxy", cleartext = true),
        KnownService(3306, "mysql", "MySQL", sensitive = true),
        KnownService(3389, "ms-wbt-server", "Remote Desktop", sensitive = true),
        KnownService(3689, "daap", "DAAP (iTunes sharing)"),
        KnownService(4443, "https-alt", "HTTPS (alternate)"),
        KnownService(4899, "radmin", "Radmin", sensitive = true),
        KnownService(5000, "upnp-or-http", "UPnP / web app", cleartext = true),
        KnownService(5001, "https-alt", "HTTPS (alternate)"),
        KnownService(5060, "sip", "SIP", cleartext = true),
        KnownService(5353, "mdns", "Multicast DNS"),
        KnownService(5357, "wsdapi", "Web Services for Devices"),
        KnownService(5432, "postgresql", "PostgreSQL", sensitive = true),
        KnownService(5540, "matter", "Matter"),
        KnownService(5631, "pcanywhere", "pcAnywhere", sensitive = true),
        KnownService(5666, "nrpe", "Nagios NRPE"),
        KnownService(5800, "vnc-http", "VNC over HTTP", cleartext = true, sensitive = true),
        KnownService(5900, "vnc", "VNC", sensitive = true),
        KnownService(5901, "vnc-1", "VNC display 1", sensitive = true),
        KnownService(5938, "teamviewer", "TeamViewer", sensitive = true),
        KnownService(5984, "couchdb", "CouchDB", sensitive = true),
        KnownService(5985, "wsman", "WinRM (HTTP)", cleartext = true, sensitive = true),
        KnownService(5986, "wsmans", "WinRM (HTTPS)", sensitive = true),
        KnownService(6000, "x11", "X11", cleartext = true, sensitive = true),
        KnownService(6053, "esphome", "ESPHome API"),
        KnownService(6379, "redis", "Redis", cleartext = true, sensitive = true),
        KnownService(6466, "androidtv-remote", "Android TV remote"),
        KnownService(6467, "androidtv-pairing", "Android TV pairing"),
        KnownService(7000, "airplay", "AirPlay"),
        KnownService(7070, "realserver", "RealServer / AnyDesk", sensitive = true),
        KnownService(7100, "airplay-alt", "AirPlay (alternate)"),
        KnownService(7474, "neo4j", "Neo4j", sensitive = true),
        KnownService(8000, "http-alt", "HTTP (alternate)", cleartext = true),
        KnownService(8008, "http-cast", "HTTP / Google Cast", cleartext = true),
        KnownService(8009, "cast", "Google Cast"),
        KnownService(8060, "roku-ecp", "Roku control", cleartext = true),
        KnownService(8080, "http-proxy", "HTTP (alternate)", cleartext = true),
        KnownService(8081, "http-alt", "HTTP (alternate)", cleartext = true),
        KnownService(8086, "influxdb", "InfluxDB", sensitive = true),
        KnownService(8088, "http-alt", "HTTP (alternate)", cleartext = true),
        KnownService(8096, "jellyfin", "Jellyfin / Emby", cleartext = true),
        KnownService(8123, "home-assistant", "Home Assistant", cleartext = true),
        KnownService(8200, "dlna", "DLNA media server"),
        KnownService(8291, "winbox", "MikroTik Winbox", sensitive = true),
        KnownService(8384, "syncthing-gui", "Syncthing web UI", cleartext = true),
        KnownService(8443, "https-alt", "HTTPS (alternate)"),
        KnownService(8883, "secure-mqtt", "MQTT over TLS"),
        KnownService(8888, "http-alt", "HTTP (alternate)", cleartext = true),
        KnownService(9000, "http-alt", "Web app / management", cleartext = true),
        KnownService(9042, "cassandra", "Cassandra", sensitive = true),
        KnownService(9090, "http-mgmt", "Management web UI", cleartext = true),
        KnownService(9100, "jetdirect", "Raw printing (JetDirect)"),
        KnownService(9200, "elasticsearch", "Elasticsearch", cleartext = true, sensitive = true),
        KnownService(9443, "https-alt", "HTTPS (alternate)"),
        KnownService(9999, "abyss", "Management / debug"),
        KnownService(10000, "webmin", "Webmin / NDMP", sensitive = true),
        KnownService(11211, "memcached", "Memcached", cleartext = true, sensitive = true),
        KnownService(21063, "homekit", "HomeKit accessory"),
        KnownService(22000, "syncthing", "Syncthing"),
        KnownService(27017, "mongodb", "MongoDB", sensitive = true),
        KnownService(28015, "rethinkdb", "RethinkDB", sensitive = true),
        KnownService(32400, "plex", "Plex Media Server"),
        KnownService(49152, "upnp-dynamic", "UPnP / dynamic"),
        KnownService(55443, "camera-rtsp", "Camera streaming"),
        KnownService(62078, "iphone-sync", "Apple device sync (lockdown)"),
    )

    private val table: Map<Int, KnownService> = all.groupBy { it.port }.mapValues { it.value.first() }
}
