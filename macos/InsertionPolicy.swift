func canInsert(currentTarget: Bool, originalMatches: Bool, frontmostMatches: Bool, secure: Bool) -> Bool {
    !secure && frontmostMatches && (currentTarget || originalMatches)
}
