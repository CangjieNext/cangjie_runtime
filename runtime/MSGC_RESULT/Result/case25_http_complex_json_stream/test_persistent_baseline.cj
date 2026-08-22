func ensure(condition: Bool): Unit {
    if (!condition) {
        throw Exception("case25 persistent baseline assertion failed")
    }
}

main() {
    ensure(!persistentBaselineEnabled(0))
    ensure(persistentBaselineEnabled(128))
    validatePersistentBaseline(0, 8)
    validatePersistentBaseline(2_048, 8)
    initializePersistentBaseline(0, 8)
}
