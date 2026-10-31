import Foundation
@main struct PolicyTests {
    static func main() {
        precondition(canInsert(currentTarget: false, originalMatches: true, frontmostMatches: true, secure: false))
        precondition(!canInsert(currentTarget: false, originalMatches: false, frontmostMatches: true, secure: false))
        precondition(canInsert(currentTarget: true, originalMatches: false, frontmostMatches: true, secure: false))
        precondition(!canInsert(currentTarget: true, originalMatches: true, frontmostMatches: false, secure: false))
        precondition(!canInsert(currentTarget: true, originalMatches: true, frontmostMatches: true, secure: true))
        print("PASS: original/completion targeting, background app rejection, secure field rejection")
    }
}
