//! [`ChatStream`]: the streaming counterpart of [`crate::ClawApiAsync::chat`].
//!
//! Wraps a transport byte stream ([`StreamingHttp::ByteStream`](claw_interface::http::StreamingHttp::ByteStream))
//! with a provider SSE parser and yields ordered [`LlmDelta`]s as tokens arrive.
//! Once drained, [`ChatStream::take_response`] returns the fully-accumulated
//! [`LlmResponse`] (text + reasoning + tool calls + reconstructed message JSON).

use core::pin::Pin;
use core::task::{Context, Poll};
use std::collections::VecDeque;

use futures_core::Stream;
use futures_lite::StreamExt;

use claw_interface::http::HttpError;

use crate::backends::sse::ProviderSse;
use crate::errors::{ChatError, ClawApiError};
use crate::types::{LlmDelta, LlmResponse};

/// A streaming chat completion.
///
/// Implements [`Stream`] over `Result<LlmDelta, ChatError>`: `Reasoning` /
/// `Output` fragments then complete `ToolCall`s, in that order. Drive it to
/// completion (a `None` item), then call [`take_response`](Self::take_response)
/// for the assembled [`LlmResponse`]. Dropping the stream cancels body reads.
///
/// `S` is the transport's owned byte stream; it (and therefore `ChatStream`) is
/// `Unpin`, so no pinning gymnastics are needed at the call site.
pub struct ChatStream<S> {
    bytes: S,
    /// `None` after the byte stream ends and the response has been assembled.
    parser: Option<ProviderSse>,
    queue: VecDeque<LlmDelta>,
    response: Option<Result<LlmResponse, ChatError>>,
}

impl<S> ChatStream<S> {
    pub(crate) fn new(bytes: S, parser: ProviderSse) -> Self {
        Self {
            bytes,
            parser: Some(parser),
            queue: VecDeque::new(),
            response: None,
        }
    }

    /// The assembled response, available once the stream has been drained to its
    /// end. Returns `None` if called before the stream finishes (or twice).
    pub fn take_response(&mut self) -> Option<Result<LlmResponse, ChatError>> {
        self.response.take()
    }
}

impl<S> Stream for ChatStream<S>
where
    S: Stream<Item = Result<Vec<u8>, HttpError>> + Unpin,
{
    type Item = Result<LlmDelta, ChatError>;

    fn poll_next(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        // ChatStream is Unpin (all fields are), so project by plain &mut.
        let this = self.get_mut();
        loop {
            if let Some(delta) = this.queue.pop_front() {
                return Poll::Ready(Some(Ok(delta)));
            }
            match Pin::new(&mut this.bytes).poll_next(cx) {
                Poll::Ready(Some(Ok(chunk))) => {
                    if let Some(parser) = &mut this.parser {
                        let mut out = Vec::new();
                        if let Err(error) = parser.push(&chunk, &mut out) {
                            return Poll::Ready(Some(Err(error)));
                        }
                        this.queue.extend(out);
                    }
                    // Loop back to drain any deltas this chunk produced.
                }
                Poll::Ready(Some(Err(error))) => {
                    return Poll::Ready(Some(Err(read_error(error))));
                }
                Poll::Ready(None) => {
                    if this.response.is_none() {
                        if let Some(parser) = this.parser.take() {
                            this.response = Some(parser.finish());
                        }
                    }
                    return Poll::Ready(None);
                }
                Poll::Pending => return Poll::Pending,
            }
        }
    }
}

/// A transport read error mid-body: the stream already started, so this is a
/// permanent (non-retryable) transport failure rather than a connect error.
fn read_error(error: HttpError) -> ChatError {
    ClawApiError::Transport(error.to_string()).into()
}

/// Drain a byte stream to a UTF-8 string. Used to read a non-2xx error body
/// before failing a streaming request.
pub(crate) async fn drain_body<S>(mut stream: S) -> String
where
    S: Stream<Item = Result<Vec<u8>, HttpError>> + Unpin,
{
    let mut buf = Vec::new();
    while let Some(Ok(chunk)) = stream.next().await {
        buf.extend_from_slice(&chunk);
    }
    String::from_utf8_lossy(&buf).into_owned()
}
