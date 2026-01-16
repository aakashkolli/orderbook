# TEST_HARNESS.md — orderbook

## How To Know When You're Done

### Component-Level Tests
Each component must pass its own test suite before the next component
begins. Run: [build command] && [test command]
Expected output: all tests pass, zero warnings in release build.

### Integration Tests
[List specific integration tests per project]
These run against real infrastructure (Kafka, Redis, Docker containers).
A component is not done until its integration test passes.

### Invariant Verification Tests
These tests directly verify the Non-Negotiable Invariants from PRD.md.
They are separate from unit tests. Run them explicitly:
[specific test command for invariant tests]

### Performance Tests
Run only after all correctness tests pass.
Required results before the project is considered complete:
[list specific numbers from PRD.md Performance Targets table]
Results must be captured in: benchmarks/results_[date].txt

### The Definition of Complete
The project is complete when:
□ All unit tests pass (zero failures)
□ All integration tests pass (zero failures)  
□ All invariant verification tests pass (zero failures)
□ All performance targets met (benchmarks/results.txt shows numbers)
□ No prohibited patterns present (grep confirms)
□ README.md has benchmark table with actual measured numbers
□ Anti-Vibe Gate answered for every component
