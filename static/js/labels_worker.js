// Module Web Worker that builds label PDFs (labels.js) off the page's main
// thread, so the dashboard stays responsive on a large sheet. The page
// fetches the part photos (its /img/ cache, shared with the reports) and
// passes them in; the backdrop versions of light parts are made here, with
// OffscreenCanvas, and kept for as long as the page keeps the worker.
//
// Runs under the CSP that /static/js/ responses carry (src/main.cpp): it
// may import this app's own scripts and nothing else.
//
// Messages in:  {id, cmd: 'labels', records, spec, hidden, maxPages, images}
//               {id, cmd: 'test_page', spec}
// Messages out: {id, progress: {phase: 'pages', page, pages}}
//               {id, pdf: Uint8Array} (transferred) or {id, error}

import { backdropJpeg } from './backdrop.js';
import { buildLabelsPdf, buildTestPage, optionsFromHidden } from './labels.js';

const tiles = new Map(); // "id|trans|light" -> Promise<Uint8Array | null>

function prepareImage(bytes, trans, light, id) {
  const key = `${id}|${trans}|${light}`;
  if (!tiles.has(key)) tiles.set(key, backdropJpeg(bytes, trans, light).catch(() => null));
  return tiles.get(key);
}

self.onmessage = async (event) => {
  const { id, cmd } = event.data;
  try {
    let pdf;
    if (cmd === 'test_page') {
      pdf = await buildTestPage(event.data.spec);
    } else if (cmd === 'labels') {
      const { records, spec, hidden, maxPages, images } = event.data;
      pdf = await buildLabelsPdf(records, spec, {
        show: optionsFromHidden(hidden),
        images,
        maxPages,
        prepareImage,
        onProgress: (p) => self.postMessage({ id, progress: p }),
      });
    } else {
      throw new Error(`unknown command ${cmd}`);
    }
    self.postMessage({ id, pdf }, [pdf.buffer]);
  } catch (e) {
    self.postMessage({ id, error: String((e && e.message) || e) });
  }
};
