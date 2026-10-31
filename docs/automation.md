# macOS build and release automation

PRs and pushes to main use the standard GitHub-hosted `macos-26` ARM64 runner. The workflow prints the compiler and SDK versions, builds the app and runs the existing unit/mocked tests through the packaging script. It then checks the ad-hoc ZIP, bundled license, fixture exclusion and resource tamper rejection. Actions are pinned to commit SHAs; checkout does not retain credentials.

PR jobs have read-only repository access and no signing secrets. They do not record audio, call model providers, request Accessibility or test actual cross-app insertion. Interactive permissions, real focus behavior and dictation still need a manual test on a Mac. Hosted builds establish compiler compatibility, not full system integration.

## Release setup awaiting owner approval

`release.yml` is deliberately disabled with an unconditional `false` job condition. It cannot sign, submit or publish anything in its present form. It is a reviewed starting point for a protected manual main-branch release, not an enabled release service.

Before enabling it:

1. Approve exporting the existing Developer ID Application certificate **and matching private key** into an encrypted `.p12`, and transferring it to GitHub environment secrets. This gives approved release jobs the ability to sign software with that identity. Base64 is encoding, not encryption.
2. Approve configuring a dedicated Apple app-specific password for CI notarization, or choose an approved Apple API-key alternative and adapt the workflow. The local Keychain profile is not automatically available on hosted runners. Never put credentials in issues, commits or workflow logs.
3. Create the `release` environment with required owner review, main-only deployment restrictions, and appropriate bypass restrictions. For a single-maintainer repository, self-review prevention would require a second reviewer; decide that rule explicitly. Repository administrators remain able to change workflows and environment protections.
4. Store environment secrets `SIGNING_P12_BASE64`, `SIGNING_P12_PASSWORD`, `APPLE_ID`, `APPLE_APP_PASSWORD`; environment variables `SIGNING_IDENTITY` (certificate SHA-1) and `APPLE_TEAM_ID`. No values are included here. Import into an ephemeral runner Keychain and delete it in an always-run cleanup step.
5. Review the exact workflow/source commit, remove the unconditional disabled condition in a separate reviewed change, and run a first approved release test. The release job needs `contents: write` only for its own repository; no tap token is configured.

The prepared workflow validates the version, builds from its exact main commit, signs, submits once, prints Apple's submission ID, waits for acceptance, checks the Apple log, staples, verifies Gatekeeper/signature and packages a fresh ZIP. It creates a **draft** release for final review; it does not publish automatically. If a submission times out or has an uncertain outcome, reconcile that ID or Apple's history before rerunning. Never blindly resubmit. A tag created before a release-creation failure also requires reconciliation before rerunning. Existing version tags are rejected; new tags are created atomically at the built commit. Rejected submissions need log inspection. Runner loss can interrupt cleanup; use ephemeral hosted runners rather than the owner's working Mac.

For a notarized PR preview, use an explicit approved manual job for a reviewed exact commit with trusted build scripts. Do not provide signing secrets to arbitrary PRs or execute a PR head with `pull_request_target`. Approval is a trust decision about the code, not a sanitizer for it. Normal PR validation stays secret-free.

A future cask update needs separate narrowly scoped access to `itomek/homebrew-lemonvox` because the built-in token is limited to the workflow repository. Until that access is separately approved, review and update the cask version, URL and final ZIP checksum manually after publishing the release.

Standard hosted runners are free for public repositories under GitHub's current policy; larger runners are unnecessary. Use bounded job timeouts and cancel superseded PR checks. Apple notarization is asynchronous and can take substantially longer than compilation. Keep the existing Developer Program membership active.

References: [GitHub runners](https://docs.github.com/en/actions/reference/runners/github-hosted-runners), [environment protection](https://docs.github.com/actions/deployment/targeting-different-environments/using-environments-for-deployment), [Apple certificates on runners](https://docs.github.com/actions/deployment/deploying-xcode-applications/installing-an-apple-certificate-on-macos-runners-for-xcode-development), [Apple notarization](https://developer.apple.com/documentation/security/customizing-the-notarization-workflow).
