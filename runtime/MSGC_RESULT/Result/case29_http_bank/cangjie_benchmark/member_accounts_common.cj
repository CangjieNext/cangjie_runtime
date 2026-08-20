func pad4(value: Int64): String {
    if (value < 10) {
        return "000${value}"
    } else if (value < 100) {
        return "00${value}"
    } else if (value < 1_000) {
        return "0${value}"
    }
    return "${value}"
}

func pad2(value: Int64): String {
    if (value < 10) {
        return "0${value}"
    }
    return "${value}"
}

func isActiveAccount(value: Int64): Bool {
    value % 5 != 0
}

func belongsToSubject(value: Int64, subjectId: String): Bool {
    let expected = if (value % 2 == 0) { "subject-A" } else { "subject-B" }
    expected == subjectId
}

func activeCardNumbers(value: Int64): String {
    let result = StringBuilder()
    var cardIndex: Int64 = 0
    var count: Int64 = 0
    while (cardIndex <= value % 3) {
        if ((value + cardIndex) % 3 != 0) {
            if (count > 0) {
                result.append(",")
            }
            result.append("card-${pad4(value)}-${cardIndex}")
            count += 1
        }
        cardIndex += 1
    }
    result.toString()
}

func appendStringField(result: StringBuilder, name: String, value: String, trailingComma: Bool): Unit {
    result.append("    \"${name}\": \"${value}\"")
    if (trailingComma) {
        result.append(",")
    }
    result.append("\n")
}

func appendMember(result: StringBuilder, value: Int64): Unit {
    let padded = pad4(value)
    let department = pad2(value % 4)
    result.append("  {\n")
    appendStringField(result, "id", "acct-${padded}", true)
    appendStringField(result, "memberId", "member-${padded}", true)
    appendStringField(result, "accountCode", "AC-${padded}", true)
    appendStringField(result, "name", "Member ${padded}", true)
    appendStringField(result, "accountDepartmentId", "dept-${department}", true)
    appendStringField(result, "accountDepartment", "Department ${department}", true)
    let cards = activeCardNumbers(value)
    result.append("    \"cardNo\": ")
    if (cards.size == 0) {
        result.append("null\n")
    } else {
        result.append("\"${cards}\"\n")
    }
    result.append("  }")
}

public func renderMembers(seed: Int64, accountCount: Int64, subjectId: String,
                          limit: Int64, offset: Int64): String {
    if (accountCount <= 0 || limit <= 0 || offset < 0) {
        return "[]"
    }
    let result = StringBuilder()
    var matched: Int64 = 0
    var emitted: Int64 = 0
    var department: Int64 = 0
    while (department < 4) {
        var index: Int64 = 0
        while (index < accountCount) {
            let value = seed + index
            if (value % 4 == department && isActiveAccount(value) && belongsToSubject(value, subjectId)) {
                if (matched >= offset && emitted < limit) {
                    if (emitted == 0) {
                        result.append("[\n")
                    } else {
                        result.append(",\n")
                    }
                    appendMember(result, value)
                    emitted += 1
                }
                matched += 1
            }
            index += 1
        }
        if (emitted == limit) {
            break
        }
        department += 1
    }
    if (emitted == 0) {
        return "[]"
    }
    result.append("\n]")
    result.toString()
}
