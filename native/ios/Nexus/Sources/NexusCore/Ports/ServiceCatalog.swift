import Foundation

public struct KnownService: Hashable, Sendable, Codable {
    public let port: UInt16
    public let name: String
    public let title: String
    public let cleartext: Bool
    public let sensitive: Bool

    public init(port: UInt16, name: String, title: String, cleartext: Bool = false, sensitive: Bool = false) {
        self.port = port
        self.name = name
        self.title = title
        self.cleartext = cleartext
        self.sensitive = sensitive
    }
}

public enum ServiceCatalog {
    public static func service(forPort port: UInt16) -> KnownService? {
        table[port]
    }

    public static func name(forPort port: UInt16) -> String? {
        table[port]?.name
    }

    public static let all: [KnownService] = [
        KnownService(port: 7, name: "echo", title: "Echo", cleartext: true),
        KnownService(port: 9, name: "discard", title: "Discard"),
        KnownService(port: 13, name: "daytime", title: "Daytime"),
        KnownService(port: 20, name: "ftp-data", title: "FTP data", cleartext: true),
        KnownService(port: 21, name: "ftp", title: "FTP", cleartext: true, sensitive: true),
        KnownService(port: 22, name: "ssh", title: "SSH", sensitive: true),
        KnownService(port: 23, name: "telnet", title: "Telnet", cleartext: true, sensitive: true),
        KnownService(port: 25, name: "smtp", title: "SMTP", cleartext: true),
        KnownService(port: 26, name: "smtp-alt", title: "SMTP (alternate)", cleartext: true),
        KnownService(port: 37, name: "time", title: "Time"),
        KnownService(port: 53, name: "domain", title: "DNS"),
        KnownService(port: 69, name: "tftp", title: "TFTP", cleartext: true, sensitive: true),
        KnownService(port: 79, name: "finger", title: "Finger", cleartext: true),
        KnownService(port: 80, name: "http", title: "HTTP", cleartext: true),
        KnownService(port: 81, name: "http-alt", title: "HTTP (alternate)", cleartext: true),
        KnownService(port: 88, name: "kerberos", title: "Kerberos"),
        KnownService(port: 106, name: "pop3pw", title: "POP3 password", cleartext: true),
        KnownService(port: 110, name: "pop3", title: "POP3", cleartext: true),
        KnownService(port: 111, name: "rpcbind", title: "RPC portmapper", sensitive: true),
        KnownService(port: 113, name: "ident", title: "Ident"),
        KnownService(port: 119, name: "nntp", title: "NNTP", cleartext: true),
        KnownService(port: 123, name: "ntp", title: "NTP"),
        KnownService(port: 135, name: "msrpc", title: "Microsoft RPC", sensitive: true),
        KnownService(port: 137, name: "netbios-ns", title: "NetBIOS name"),
        KnownService(port: 139, name: "netbios-ssn", title: "NetBIOS session", sensitive: true),
        KnownService(port: 143, name: "imap", title: "IMAP", cleartext: true),
        KnownService(port: 144, name: "news", title: "NeWS"),
        KnownService(port: 179, name: "bgp", title: "BGP"),
        KnownService(port: 199, name: "smux", title: "SNMP multiplexer"),
        KnownService(port: 389, name: "ldap", title: "LDAP", cleartext: true, sensitive: true),
        KnownService(port: 427, name: "svrloc", title: "Service Location Protocol"),
        KnownService(port: 443, name: "https", title: "HTTPS"),
        KnownService(port: 444, name: "snpp", title: "SNPP"),
        KnownService(port: 445, name: "microsoft-ds", title: "SMB", sensitive: true),
        KnownService(port: 465, name: "smtps", title: "SMTP over TLS"),
        KnownService(port: 513, name: "login", title: "rlogin", cleartext: true, sensitive: true),
        KnownService(port: 514, name: "shell", title: "rsh", cleartext: true, sensitive: true),
        KnownService(port: 515, name: "printer", title: "LPD printing"),
        KnownService(port: 543, name: "klogin", title: "Kerberos login"),
        KnownService(port: 544, name: "kshell", title: "Kerberos shell"),
        KnownService(port: 548, name: "afp", title: "Apple Filing Protocol", sensitive: true),
        KnownService(port: 554, name: "rtsp", title: "RTSP streaming", cleartext: true),
        KnownService(port: 587, name: "submission", title: "Mail submission"),
        KnownService(port: 591, name: "http-alt", title: "HTTP (FileMaker)", cleartext: true),
        KnownService(port: 631, name: "ipp", title: "Internet Printing Protocol"),
        KnownService(port: 646, name: "ldp", title: "LDP"),
        KnownService(port: 873, name: "rsync", title: "rsync", cleartext: true, sensitive: true),
        KnownService(port: 990, name: "ftps", title: "FTP over TLS"),
        KnownService(port: 993, name: "imaps", title: "IMAP over TLS"),
        KnownService(port: 995, name: "pop3s", title: "POP3 over TLS"),
        KnownService(port: 1025, name: "nfs-or-iis", title: "Dynamic RPC"),
        KnownService(port: 1110, name: "nfsd-status", title: "NFS status"),
        KnownService(port: 1400, name: "sonos", title: "Sonos control", cleartext: true),
        KnownService(port: 1433, name: "ms-sql-s", title: "Microsoft SQL Server", sensitive: true),
        KnownService(port: 1521, name: "oracle", title: "Oracle database", sensitive: true),
        KnownService(port: 1720, name: "h323q931", title: "H.323"),
        KnownService(port: 1723, name: "pptp", title: "PPTP VPN", sensitive: true),
        KnownService(port: 1755, name: "wms", title: "Windows Media"),
        KnownService(port: 1883, name: "mqtt", title: "MQTT", cleartext: true, sensitive: true),
        KnownService(port: 1900, name: "upnp", title: "UPnP / SSDP"),
        KnownService(port: 2000, name: "cisco-sccp", title: "Cisco SCCP"),
        KnownService(port: 2049, name: "nfs", title: "NFS", sensitive: true),
        KnownService(port: 2121, name: "ccproxy-ftp", title: "FTP (alternate)", cleartext: true),
        KnownService(port: 2222, name: "ssh-alt", title: "SSH (alternate)", sensitive: true),
        KnownService(port: 3000, name: "http-dev", title: "Web app (development)", cleartext: true),
        KnownService(port: 3128, name: "squid-http", title: "HTTP proxy", cleartext: true),
        KnownService(port: 3306, name: "mysql", title: "MySQL", sensitive: true),
        KnownService(port: 3389, name: "ms-wbt-server", title: "Remote Desktop", sensitive: true),
        KnownService(port: 3689, name: "daap", title: "DAAP (iTunes sharing)"),
        KnownService(port: 4443, name: "https-alt", title: "HTTPS (alternate)"),
        KnownService(port: 4899, name: "radmin", title: "Radmin", sensitive: true),
        KnownService(port: 5000, name: "upnp-or-http", title: "UPnP / web app", cleartext: true),
        KnownService(port: 5001, name: "https-alt", title: "HTTPS (alternate)"),
        KnownService(port: 5060, name: "sip", title: "SIP", cleartext: true),
        KnownService(port: 5353, name: "mdns", title: "Multicast DNS"),
        KnownService(port: 5357, name: "wsdapi", title: "Web Services for Devices"),
        KnownService(port: 5432, name: "postgresql", title: "PostgreSQL", sensitive: true),
        KnownService(port: 5540, name: "matter", title: "Matter"),
        KnownService(port: 5631, name: "pcanywhere", title: "pcAnywhere", sensitive: true),
        KnownService(port: 5666, name: "nrpe", title: "Nagios NRPE"),
        KnownService(port: 5800, name: "vnc-http", title: "VNC over HTTP", cleartext: true, sensitive: true),
        KnownService(port: 5900, name: "vnc", title: "VNC", sensitive: true),
        KnownService(port: 5901, name: "vnc-1", title: "VNC display 1", sensitive: true),
        KnownService(port: 5938, name: "teamviewer", title: "TeamViewer", sensitive: true),
        KnownService(port: 5984, name: "couchdb", title: "CouchDB", sensitive: true),
        KnownService(port: 5985, name: "wsman", title: "WinRM (HTTP)", cleartext: true, sensitive: true),
        KnownService(port: 5986, name: "wsmans", title: "WinRM (HTTPS)", sensitive: true),
        KnownService(port: 6000, name: "x11", title: "X11", cleartext: true, sensitive: true),
        KnownService(port: 6053, name: "esphome", title: "ESPHome API"),
        KnownService(port: 6379, name: "redis", title: "Redis", cleartext: true, sensitive: true),
        KnownService(port: 6466, name: "androidtv-remote", title: "Android TV remote"),
        KnownService(port: 6467, name: "androidtv-pairing", title: "Android TV pairing"),
        KnownService(port: 7000, name: "airplay", title: "AirPlay"),
        KnownService(port: 7070, name: "realserver", title: "RealServer / AnyDesk", sensitive: true),
        KnownService(port: 7100, name: "airplay-alt", title: "AirPlay (alternate)"),
        KnownService(port: 7474, name: "neo4j", title: "Neo4j", sensitive: true),
        KnownService(port: 8000, name: "http-alt", title: "HTTP (alternate)", cleartext: true),
        KnownService(port: 8008, name: "http-cast", title: "HTTP / Google Cast", cleartext: true),
        KnownService(port: 8009, name: "cast", title: "Google Cast"),
        KnownService(port: 8060, name: "roku-ecp", title: "Roku control", cleartext: true),
        KnownService(port: 8080, name: "http-proxy", title: "HTTP (alternate)", cleartext: true),
        KnownService(port: 8081, name: "http-alt", title: "HTTP (alternate)", cleartext: true),
        KnownService(port: 8086, name: "influxdb", title: "InfluxDB", sensitive: true),
        KnownService(port: 8088, name: "http-alt", title: "HTTP (alternate)", cleartext: true),
        KnownService(port: 8096, name: "jellyfin", title: "Jellyfin / Emby", cleartext: true),
        KnownService(port: 8123, name: "home-assistant", title: "Home Assistant", cleartext: true),
        KnownService(port: 8200, name: "dlna", title: "DLNA media server"),
        KnownService(port: 8291, name: "winbox", title: "MikroTik Winbox", sensitive: true),
        KnownService(port: 8384, name: "syncthing-gui", title: "Syncthing web UI", cleartext: true),
        KnownService(port: 8443, name: "https-alt", title: "HTTPS (alternate)"),
        KnownService(port: 8883, name: "secure-mqtt", title: "MQTT over TLS"),
        KnownService(port: 8888, name: "http-alt", title: "HTTP (alternate)", cleartext: true),
        KnownService(port: 9000, name: "http-alt", title: "Web app / management", cleartext: true),
        KnownService(port: 9042, name: "cassandra", title: "Cassandra", sensitive: true),
        KnownService(port: 9090, name: "http-mgmt", title: "Management web UI", cleartext: true),
        KnownService(port: 9100, name: "jetdirect", title: "Raw printing (JetDirect)"),
        KnownService(port: 9200, name: "elasticsearch", title: "Elasticsearch", cleartext: true, sensitive: true),
        KnownService(port: 9443, name: "https-alt", title: "HTTPS (alternate)"),
        KnownService(port: 9999, name: "abyss", title: "Management / debug"),
        KnownService(port: 10000, name: "webmin", title: "Webmin / NDMP", sensitive: true),
        KnownService(port: 11211, name: "memcached", title: "Memcached", cleartext: true, sensitive: true),
        KnownService(port: 21063, name: "homekit", title: "HomeKit accessory"),
        KnownService(port: 22000, name: "syncthing", title: "Syncthing"),
        KnownService(port: 27017, name: "mongodb", title: "MongoDB", sensitive: true),
        KnownService(port: 28015, name: "rethinkdb", title: "RethinkDB", sensitive: true),
        KnownService(port: 32400, name: "plex", title: "Plex Media Server"),
        KnownService(port: 49152, name: "upnp-dynamic", title: "UPnP / dynamic"),
        KnownService(port: 55443, name: "camera-rtsp", title: "Camera streaming"),
        KnownService(port: 62078, name: "iphone-sync", title: "Apple device sync (lockdown)"),
    ]

    static let table: [UInt16: KnownService] = Dictionary(all.map { ($0.port, $0) }, uniquingKeysWith: { first, _ in first })
}
