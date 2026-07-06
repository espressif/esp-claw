use core::future::Future;

use edge_executor::LocalExecutor;
use futures_lite::future::block_on;

pub(crate) fn run<F>(future: F)
where
    F: Future<Output = ()>,
{
    let executor = LocalExecutor::<4>::new();
    block_on(executor.run(future));
}
