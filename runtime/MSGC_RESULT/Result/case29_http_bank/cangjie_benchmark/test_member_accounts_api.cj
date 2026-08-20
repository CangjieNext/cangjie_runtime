func ensureCase29(actual: String, expected: String, name: String): Unit {
    if (actual != expected) {
        throw Exception("${name} mismatch: ${actual}")
    }
}

main() {
    let defaultPage = """
[
  {
    \"id\": \"acct-0004\",
    \"memberId\": \"member-0004\",
    \"accountCode\": \"AC-0004\",
    \"name\": \"Member 0004\",
    \"accountDepartmentId\": \"dept-00\",
    \"accountDepartment\": \"Department 00\",
    \"cardNo\": \"card-0004-0,card-0004-1\"
  },
  {
    \"id\": \"acct-0008\",
    \"memberId\": \"member-0008\",
    \"accountCode\": \"AC-0008\",
    \"name\": \"Member 0008\",
    \"accountDepartmentId\": \"dept-00\",
    \"accountDepartment\": \"Department 00\",
    \"cardNo\": \"card-0008-0,card-0008-2\"
  },
  {
    \"id\": \"acct-0002\",
    \"memberId\": \"member-0002\",
    \"accountCode\": \"AC-0002\",
    \"name\": \"Member 0002\",
    \"accountDepartmentId\": \"dept-02\",
    \"accountDepartment\": \"Department 02\",
    \"cardNo\": \"card-0002-0,card-0002-2\"
  },
  {
    \"id\": \"acct-0006\",
    \"memberId\": \"member-0006\",
    \"accountCode\": \"AC-0006\",
    \"name\": \"Member 0006\",
    \"accountDepartmentId\": \"dept-02\",
    \"accountDepartment\": \"Department 02\",
    \"cardNo\": null
  }
]"""
    ensureCase29(renderMembers(0, 10, "subject-A", 50, 0), defaultPage, "default page")
    ensureCase29(renderMembers(0, 10, "missing", 50, 0), "[]", "unknown subject")
    ensureCase29(renderMembers(0, 10, "subject-A", 0, 0), "[]", "zero limit")
    println("PASS case29 Cangjie contract")
}
