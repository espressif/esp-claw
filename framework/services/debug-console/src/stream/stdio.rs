//! Standard input/output byte stream.

use std::io::{self, Read, Write};

use crate::{ByteStream, Error, Result};

/// Byte stream backed by process standard input and output.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Stdio;

impl ByteStream for Stdio {
    fn read(&mut self, bytes: &mut [u8]) -> Result<usize> {
        let mut stdin = io::stdin();
        stdin.read(bytes).map_err(map_io_error)
    }

    fn write(&mut self, bytes: &[u8]) -> Result<usize> {
        let mut stdout = io::stdout();
        stdout.write(bytes).map_err(map_io_error)
    }

    fn flush(&mut self) -> Result<()> {
        let mut stdout = io::stdout();
        stdout.flush().map_err(map_io_error)
    }
}

fn map_io_error(error: io::Error) -> Error {
    Error::Stream(error.to_string())
}
