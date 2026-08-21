package cangjie_member_query_service

import std.collection.*
import std.convert.*
import std.database.sql.*
import std.env.*
import std.io.*
import stdx.encoding.json.stream.*
import stdx.net.http.*
import mariadb.cdbc.*
import stdx.encoding.url.*

private const ServiceName = "cangjie-member-query"
private const DefaultListenURL = "http://0.0.0.0:18081"
private const DefaultJavaServiceUrl = "http://canteen"

main(args: Array<String>): Int64 {
    let listenURL = getVariable("SERVER_URL") ?? DefaultListenURL
    let javaServiceUrl = getVariable("JAVA_SERVICE_URL") ?? DefaultJavaServiceUrl
    let instanceToken = getVariable("INSTANCE_TOKEN") ?? String.empty
    let connectionString = getVariable("ConnectionString") ?? throw NoneValueException("ConnectionString")
    let dataSource = createDataSource(connectionString)

    let client = ClientBuilder().build()

    let parsedListenURL = URL.parse(listenURL)
    let builder = ServerBuilder().addr(parsedListenURL.hostName).port(UInt16.parse(parsedListenURL.port))
    let server = builder.build()

    server.distributor.register(
        "/health",
        {
            context =>
                addCommonJsonHeaders(context)
                context.responseBuilder.header("X-Instance-Token", instanceToken)
                unsafe {
                    context.responseBuilder.body("{\"status\":\"UP\",\"service\":\"cangjie-member-query\"}")
                }
        }
    )

    server.distributor.register(
        "/hello",
        {
            context =>
                addCommonJsonHeaders(context)
                unsafe {
                    context
                        .responseBuilder
                        .body("{\"ok\":true,\"service\":\"cangjie-member-query\",\"language\":\"Cangjie\"}")
                }
        }
    )

    server.distributor.register(
        "/api/cangjie/call-java",
        {
            context =>
                let target = "${javaServiceUrl}/api/java/hello"
                let response = client.get(target)
                let body = readToEnd(response.body)
                addCommonJsonHeaders(context)
                context.responseBuilder.header("X-Demo-Caller", ServiceName)
                context.responseBuilder.header("X-Downstream-Status", response.status.toString())
                context.responseBuilder.status(response.status)
                context.responseBuilder.body(body)
        }
    )

    server.distributor.register(
        "/api/v1/member-accounts",
        {
            context =>
                let subjectId = context.request.url.queryForm.get("subjectId") ?? "0"
                let limit = context.request.url.queryForm.get("limit") ?? "10"
                let offset = context.request.url.queryForm.get("offset") ?? "0"
                let result = MemberAccount.queryMemberAccounts(dataSource, subjectId, Int64.parse(limit),
                    Int64.parse(offset))
                addCommonJsonHeaders(context)
                let ms = ByteBuffer()
                let jw = JsonWriter(ms)
                result.toJson(jw)
                jw.flush()
                context.responseBuilder.body(ms.bytes())
        }
    )

    println("[cangjie-member-query] listening on ${listenURL}")
    println("[cangjie-member-query] native stdx HTTP; MySQL configuration loaded")
    server.serve()
    return 0
}

class MemberAccount <: JsonSerializable {
    private static let QUERY_SQL = """
SELECT
    COALESCE(CAST(ma.id AS CHAR), '') AS id,
    COALESCE(CAST(ma.member_id AS CHAR), '') AS member_id,
    COALESCE(CAST(ma.account_code AS CHAR), '') AS account_code,
    COALESCE(CAST(ma.name AS CHAR), '') AS name,
    COALESCE(CAST(ma.account_department_id AS CHAR), '') AS account_department_id,
    COALESCE(CAST(ma.account_department AS CHAR), '') AS account_department,
    COALESCE(group_concat(mc.card_no ORDER BY mc.id ASC SEPARATOR ','), '') AS card_no
FROM member_account ma
LEFT JOIN member_card mc
    ON mc.account_id = ma.id AND mc.status = 1
WHERE ma.status = 1
  AND ma.ower_info_subject_id = ?
GROUP BY ma.id
ORDER BY ma.account_department_id, ma.name, ma.id
LIMIT ? OFFSET ?
"""
    public var id: String = ""
    public var memberId: String = ""
    public var accountCode: String = ""
    public var name: String = ""
    public var accountDepartmentId: String = ""
    public var accountDepartment: String = ""
    public var cardNo: String = ""

    public func toJson(w: JsonWriter): Unit {
        w.startObject()
        w.writeName("id")
        w.writeValue(id)
        w.writeName("memberId")
        w.writeValue(memberId)
        w.writeName("accountCode")
        w.writeValue(accountCode)
        w.writeName("name")
        w.writeValue(name)
        w.writeName("accountDepartmentId")
        w.writeValue(accountDepartmentId)
        w.writeName("accountDepartment")
        w.writeValue(accountDepartment)
        w.writeName("cardNo")
        w.writeValue(cardNo)
        w.endObject()
    }

    public static func queryMemberAccounts(dataSource: Datasource, subjectId: String, limit: Int64, offset: Int64) {
        let list = ArrayList<MemberAccount>(32)
        try (connection = dataSource.connect()) {
            try (statement = connection.prepareStatement(QUERY_SQL)) {
                statement.set<String>(1, subjectId)
                statement.set<Int64>(2, limit)
                statement.set<Int64>(3, offset)
                try (result = statement.query()) {
                    while (result.next()) {
                        let item = MemberAccount()
                        item.id = result.getOrNull<String>(1) ?? ""
                        item.memberId = result.getOrNull<String>(2) ?? ""
                        item.accountCode = result.getOrNull<String>(3) ?? ""
                        item.name = result.getOrNull<String>(4) ?? ""
                        item.accountDepartmentId = result.getOrNull<String>(5) ?? ""
                        item.accountDepartment = result.getOrNull<String>(6) ?? ""
                        item.cardNo = result.getOrNull<String>(7) ?? ""
                        list.add(item)
                    }
                }
            }
        }
        return list
    }
}

private func readToEnd(input: InputStream): Array<Byte> {
    let result = ByteBuffer(8192)
    let buffer = Array<Byte>(4096, repeat: 0)
    var length = input.read(buffer)
    while (length > 0) {
        result.write(buffer[0..length])
        length = input.read(buffer)
    }
    return result.bytes()
}

private func addCommonJsonHeaders(context: HttpContext): Unit {
    context.responseBuilder.header("Content-Type", "application/json; charset=utf-8")
    context.responseBuilder.header("Cache-Control", "no-store")
    context.responseBuilder.header("X-Demo-Service", ServiceName)
}

private func createDataSource(connectionString: String): Datasource {
    let driver = DriverManager.getDriver("mariadb").getOrThrow()
    let dataSource = driver.open(connectionString)
    // The driver's implementation defaults to server-side prepare despite its
    // parameter comment.  This benchmark closes every statement per request;
    // use client prepare to avoid issuing a COM_STMT_CLOSE on the MySQL socket
    // from the HTTP worker's teardown path.
    dataSource.setOption("prepare.mode", "client")
    return dataSource
}
