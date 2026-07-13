use crate::{DurablePartError, PartGeneration, PartStateBlob, PartStateSlice};

/// Encoding contract for state stored inside [`DurableState`].
pub trait DurableStateCodec {
    /// Encode this durable state into a checkpoint payload.
    fn encode_state(&self) -> Result<PartStateBlob<'_>, DurablePartError>;

    /// Decode this durable state from a checkpoint payload.
    fn decode_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError>
    where
        Self: Sized;
}

/// A durable state cell that owns a part-local generation counter.
///
/// Mutating access through [`get_mut`](Self::get_mut) bumps the generation before
/// returning the inner state. This may mark a part dirty even if the caller does
/// not ultimately change a field, but it keeps durable mutations explicit.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DurableState<T: DurableStateCodec> {
    inner: T,
    generation: PartGeneration,
}

impl<T: DurableStateCodec> DurableState<T> {
    pub fn new(inner: T) -> Self {
        Self {
            inner,
            generation: 0,
        }
    }

    pub fn generation(&self) -> PartGeneration {
        self.generation
    }

    pub fn get(&self) -> &T {
        &self.inner
    }

    pub fn get_mut(&mut self) -> &mut T {
        self.bump_generation();
        &mut self.inner
    }

    pub fn replace(&mut self, inner: T) {
        self.inner = inner;
        self.bump_generation();
    }

    pub fn export_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        self.inner.encode_state()
    }

    pub fn restore_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError> {
        Ok(Self::new(T::decode_state(state)?))
    }

    fn bump_generation(&mut self) {
        self.generation = self.generation.saturating_add(1);
    }
}

impl<T: DurableStateCodec + Default> Default for DurableState<T> {
    fn default() -> Self {
        Self::new(T::default())
    }
}
