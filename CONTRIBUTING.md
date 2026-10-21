# Contributing to WineForge

Thank you for contributing to WineForge. Bug reports, focused fixes, tests, and
documentation improvements are welcome.

WineForge is a macOS-focused Wine project that prioritizes upstream compatibility,
stability, maintainability, and minimal modifications to the original Wine
codebase. Contributions should support these priorities while preserving
compatibility beyond the application that motivated a change.

These rules apply equally to every contributor, including the project maintainer.
See the [README](README.md) for project context and build information.

## 1. Upstream first

- Stay as close as possible to upstream Wine. Prefer fixes that fit upstream
  behavior and architecture over custom implementations.
- Avoid replacing existing Wine functionality unless there is a clearly
  demonstrated technical necessity. Explain the limitation and provide evidence
  that the existing implementation cannot reasonably address it.
- Consider submitting generally applicable fixes directly to upstream Wine or
  the relevant upstream dependency. Describe any upstream discussion or submission
  associated with the contribution.
- Do not introduce unnecessary architectural changes. Any divergence from
  upstream should have a specific purpose and a justified scope.

## 2. Minimal and isolated changes

- Keep patches small, focused, and independently reviewable.
- A fix for one game must not unnecessarily modify shared Wine components.
  When a shared change is necessary, explain the underlying defect and keep the
  modification limited to what the fix requires.
- Prefer isolated implementations with minimal integration points when
  technically appropriate. Avoid rewriting existing subsystems to solve isolated
  compatibility problems.
- Make changes reversible without breaking unrelated functionality whenever
  possible. Explain any dependencies that prevent independent removal.
- Before proposing a major rewrite, explain why a smaller solution is
  insufficient and document the alternatives considered.

## 3. API correctness

- Follow documented Windows API behavior and contracts. Cite the relevant
  documentation when it helps establish the expected behavior.
- Do not implement functionality solely to satisfy the behavior of one specific
  game. Consider other valid uses of the same API.
- Correctly handle error codes, edge cases, resource lifetimes, memory management,
  and synchronization.
- Returning success without implementing the required behavior is unacceptable.
  Capability reporting must reflect what the implementation actually supports.
- Partial implementations must clearly document their supported behavior,
  limitations, and unsupported cases. They must not conceal missing behavior
  behind a successful return value.

## 4. Regression prevention

- Consider how the change affects other games, applications, architectures, and
  system configurations relevant to the affected code.
- Testing a single game is not sufficient evidence that a shared subsystem
  modification is safe. Include checks of the affected API and adjacent behavior
  appropriate to the scope and risk of the change.
- Provide regression tests whenever practical. If an automated test is not
  practical, explain why and provide reproducible manual validation steps.
- Clearly distinguish tested behavior from assumptions. Document known
  limitations, remaining risks, and untested scenarios.
- Performance improvements must include reproducible benchmarks and testing
  methodology. Identify the baseline and modified revisions, relevant hardware,
  macOS version, runtime configuration, workload, commands, and measurement method.
  Compare equivalent conditions and report repeated measurements and their
  variation where practical.

## 5. AI-assisted contributions

AI-assisted development is permitted. All contributions are evaluated against
the same technical standards, regardless of how the code was written.

- Contributors remain fully responsible for every submitted change.
- Review and understand AI-generated code before submission. Be able to explain
  the implementation, architectural decisions, and potential side effects.
- A successful build or a working game does not establish correctness.
- Do not submit large AI-generated rewrites without reviewing and understanding
  their impact on the wider Wine codebase.

## 6. Pull request requirements

Every significant pull request should explain:

1. What problem is being solved, including how to reproduce it.
2. What causes the original problem and what evidence supports that diagnosis.
3. Why the proposed implementation is necessary.
4. Which Wine components are affected and how their behavior changes.
5. Which alternatives were considered and why they were not chosen.
6. What testing was performed, with enough detail to reproduce the results.
7. What regression risks, limitations, and untested scenarios remain.
8. How the change can be reverted or disabled, including any required cleanup.

Scale the explanation to the complexity of the change. A small documentation
correction does not need the same analysis as a change to a shared subsystem.

Submit unrelated changes separately. Do not combine independent modifications
to components such as wineserver, audio, graphics, or synchronization in a single
pull request without strong technical justification. If one fix requires changes
across multiple components, explain their relationship and why they must be
reviewed together.

## 7. Long-term maintainability

- Consider compatibility with future upstream Wine releases and the work needed
  to carry the change forward.
- Avoid unnecessary dependencies between custom patches. Prefer solutions that
  can be independently updated, tested, and removed.
- Minimize the maintenance burden of each contribution. Keep the implementation
  understandable and document behavior that differs from upstream.
- Do not introduce game-specific workarounds into shared components without clear
  justification. Explain why a general correction or a more isolated solution is
  insufficient and how unrelated applications retain their expected behavior.

## 8. Maintainer discretion

The WineForge maintainer reserves the right to request changes or reject pull
requests that:

- Introduce excessive maintenance overhead.
- Unnecessarily diverge from upstream Wine.
- Lack sufficient technical justification.
- Modify unrelated components.
- Introduce unacceptable regression risks.
- Conflict with the project's architectural direction.

A patch may be rejected even if it improves performance or fixes a specific
game. Working code is not automatically suitable for inclusion. Acceptance also
depends on correctness, scope, compatibility, and long-term maintainability.

The maintainer's own contributions are subject to these same requirements.
Review feedback should address the implementation and supporting evidence, with
the goal of helping contributors produce changes the project can maintain.
