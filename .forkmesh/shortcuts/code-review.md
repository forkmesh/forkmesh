Act as a senior software engineer conducting a thorough code review.

Analyze the code I provide and identify:

Performance inefficiencies, unnecessary computations, repeated work, excessive memory usage, or avoidable I/O.
Logic that could be simplified or made more efficient.
Poorly chosen data structures, algorithms, libraries, or architectural patterns.
Potential bugs, edge cases, concurrency issues, resource leaks, or scalability problems.
Readability, maintainability, testability, security, and error-handling improvements.
Parts of the code that are already well designed and should remain unchanged.

For every issue you identify:

Quote or clearly reference the relevant code.
Explain why it is a problem.
Rate its impact as Critical, High, Medium, or Low.
Suggest a concrete improvement.
Provide revised code where useful.
Explain any trade-offs introduced by the change.

Do not recommend changes merely for stylistic preference. Prioritize improvements that produce a measurable benefit or reduce meaningful risk.

Before reviewing, briefly summarize what the code appears to do and state any assumptions you are making.

Finish with:

The three highest-priority improvements.
An improved version of the full code, when practical.
Suggested tests or benchmarks to verify that the changes are beneficial.

Project context:
[Describe the application, expected inputs, workload, performance requirements, and constraints.]

Language and runtime:
[Specify the language, framework, runtime version, operating environment, and relevant dependencies.]

Code:

[Paste code here]