import Foundation
@main struct PolicyTests {
    static func main() {
        for currentTarget in [false, true] {
            for originalMatches in [false, true] {
                for frontmostMatches in [false, true] {
                    for secure in [false, true] {
                        let expected = !secure && frontmostMatches && (currentTarget || originalMatches)
                        precondition(canInsert(currentTarget: currentTarget, originalMatches: originalMatches,
                            frontmostMatches: frontmostMatches, secure: secure) == expected)
                        var facts = LVInsertionFacts()
                        facts.current_target = currentTarget ? 1 : 0
                        facts.original_matches = originalMatches ? 1 : 0
                        facts.frontmost_matches = frontmostMatches ? 1 : 0
                        facts.secure = secure ? 1 : 0
                        precondition((lv_can_insert(&facts) != 0) == expected)
                    }
                }
            }
        }
        print("PASS: original/current targeting, background rejection and secure-field rejection")
    }
}
