/// Messages created outside the LLM stream and still awaiting engine emission.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct DriveOutput {
    messages: Vec<String>,
}

impl DriveOutput {
    pub(super) fn absorb(&mut self, other: DriveOutput) {
        self.messages.extend(other.messages);
    }

    pub(crate) fn message(message: String) -> Self {
        Self {
            messages: vec![message],
        }
    }

    pub(crate) fn into_messages(self) -> Vec<String> {
        self.messages
    }
}

#[cfg(test)]
mod tests {
    use super::DriveOutput;

    #[test]
    fn drive_output_owns_only_messages_that_still_need_emission() {
        let mut output = DriveOutput::message("first".to_owned());
        output.absorb(DriveOutput::message("second".to_owned()));

        assert_eq!(
            output.into_messages(),
            vec!["first".to_owned(), "second".to_owned()]
        );
    }
}
