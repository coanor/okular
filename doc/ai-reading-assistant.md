# AI reading assistant (desktop)

Open a document and choose **View → AI Reading Assistant**. Add a model in **Models…**, then ask a question about the current page. For a selected passage, right-click it and choose **Ask AI about Selected Text**; the panel opens with the selection as context, and waits for a question.

Each question sends the current page's extracted text when available. Vision profiles also send a JPEG rendering of the current page. Page content is sent only after the reader presses **Ask**. The assistant may use its own knowledge to answer; it is not limited to passages in the document. The first version does not search the whole book.

## Model profiles

- **OpenAI-compatible Chat Completions:** base URL usually ends in `/v1`; requires a model name and API key. History is replayed from Okular's local record.
- **Anthropic-compatible Messages:** base URL normally points to the service root; requires a model name and API key. History is replayed from Okular's local record.
- **OpenAI Responses:** uses `/v1/conversations` and `/v1/responses` so the service keeps conversation state. Requires a supporting endpoint, model and API key.
- **Local Codex CLI:** uses the installed, signed-in `codex` command and resumes the saved Codex session ID. No API key is entered in Okular. The Codex process runs with a read-only sandbox.

Mark **This model accepts page images** only if that model supports image input. Text-only profiles require selected text to ask about a page.

API keys are saved in KWallet when available; otherwise they remain in memory for this Okular process. Named profiles and conversations are local. Conversations are separated by document content hash and profile ID. The local conversation record contains questions, answers, selected text, extracted page text and the most recent page image. When a new question is sent through a stateless API, earlier page images are discarded while earlier text stays in the request history. Responses and Codex store a remote session ID as well; their providers keep the full conversation.

**Start new conversation** clears the active document/profile conversation from Okular. It does not delete the provider's history or saved annotations.

## Answers and annotations

Answers render offline with Markdown and KaTeX math (`$…$`, `$$…$$`, `\(…\)`, `\[…\]`). Raw HTML and Mermaid are not supported. Remote images are shown as links; external links open in the browser.

Choose **Save as annotation** under an answer and click a point on the answer's PDF page. Okular creates a note containing the question and answer and saves it to the local PDF annotation sidecar. The original PDF bytes remain unchanged. This action is available for local PDFs only. See [annotation-sidecars.md](annotation-sidecars.md) for the sidecar format and location.
