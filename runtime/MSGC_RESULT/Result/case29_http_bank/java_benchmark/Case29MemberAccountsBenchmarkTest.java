public final class Case29MemberAccountsBenchmarkTest {
    private static final String DEFAULT_PAGE = """
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
        ]
        """.stripTrailing();

    private static void requireEquals(String expected, String actual, String name) {
        if (!expected.equals(actual)) {
            throw new AssertionError(name + " mismatch:\nexpected=" + expected + "\nactual=" + actual);
        }
    }

    public static void main(String[] args) {
        requireEquals(DEFAULT_PAGE,
            Case29MemberAccountsBenchmark.renderMembers(0, 10, "subject-A", 50, 0),
            "default page");
        requireEquals("""
            [
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
              }
            ]
            """.stripTrailing(),
            Case29MemberAccountsBenchmark.renderMembers(0, 10, "subject-A", 2, 1),
            "offset page");
        requireEquals("[]", Case29MemberAccountsBenchmark.renderMembers(0, 10, "missing", 50, 0),
            "unknown subject");
        requireEquals("[]", Case29MemberAccountsBenchmark.renderMembers(0, 10, "subject-A", 0, 0),
            "zero limit");
        if (!Case29MemberAccountsBenchmark.runHttpSelfTest()) {
            throw new AssertionError("HTTP self test failed");
        }
        System.out.println("PASS Case29MemberAccountsBenchmarkTest");
    }
}
