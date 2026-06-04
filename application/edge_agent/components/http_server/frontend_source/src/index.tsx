/* @refresh reload */
import { render } from 'solid-js/web';
import './index.css';

import App from './App';
import { installUnsavedGuard } from './state/dirty';

const root = document.getElementById('root');

if (import.meta.env.DEV && !(root instanceof HTMLElement)) {
  throw new Error('Root element not found. Did you forget to add it to your index.html?');
}

installUnsavedGuard();

render(() => <App />, root!);
