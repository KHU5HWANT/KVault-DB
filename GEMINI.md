# Technical Discussion Logging Rule

Whenever the user and the agent engage in a technical discussion about the codebase, architecture, system design, or code implementations, the agent MUST summarize the key technical points and append them to `chat_discussion.txt` in the root of the project.

- DO NOT append general conversation, greetings, or non-technical chatter.
- ONLY append substantial technical insights, code decisions, architectural rules, or debugging takeaways.
- Use the `run_command` tool (e.g., `echo "..." >> chat_discussion.txt`) or `multi_replace_file_content` / `replace_file_content` tools to append to the file.
- Do this proactively without the user needing to ask.

# Documentation Auto-Update Rule

Whenever changes are made to the project's codebase, the agent MUST automatically update `KVault_Internals.tex` to reflect these changes, without being explicitly told by the user.

- Since `KVault_Internals.tex` is a LaTeX document, WARNING: Do NOT simply append text to the very end of the file. This will place text after `\end{document}` and break compilation.
- You MUST insert any new changes or sections BEFORE the `\end{document}` tag.
- Ensure the updates are formatted in valid LaTeX and maintain the highly technical, academic tone of the existing book.
