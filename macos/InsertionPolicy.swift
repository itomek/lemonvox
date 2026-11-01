func canInsert(currentTarget: Bool, originalMatches: Bool, frontmostMatches: Bool, secure: Bool) -> Bool {
    var facts = LVInsertionFacts()
    facts.current_target = currentTarget ? 1 : 0
    facts.original_matches = originalMatches ? 1 : 0
    facts.frontmost_matches = frontmostMatches ? 1 : 0
    facts.secure = secure ? 1 : 0
    return lv_can_insert(&facts) != 0
}
