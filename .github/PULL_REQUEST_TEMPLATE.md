## What & why

<!-- Describe the change and the problem it solves. Link the issue: "Closes #123". -->

## Type

- [ ] Bug fix
- [ ] New feature
- [ ] Protocol conformance fix
- [ ] Documentation
- [ ] Build / packaging

## Checklist

- [ ] Tests added or updated (`ctest --test-dir build --output-on-failure` passes)
- [ ] Build is warning-free (`-Wall -Wextra -Wpedantic` / `/W4`)
- [ ] Public headers stay dependency-clean (no third-party includes beyond the bundled JSON header)
- [ ] Docs updated (README / ROADMAP / CHANGELOG as applicable)
- [ ] No new runtime dependency (or an issue approved it)

## Notes for reviewers

<!-- Anything protocol-sensitive, or trade-offs you made. -->
