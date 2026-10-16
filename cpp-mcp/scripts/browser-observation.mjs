const loading = new Set(['loading', 'insufficient_content', 'navigation_unstable']);
const dataOf = response => response?.structuredContent?.data;
const imageOf = response => response?.content?.find(item => item.type === 'image');

// Retain inspectable evidence if a later observation fails. The caller must
// still treat error as a failed workflow and avoid input on the old snapshot.
export async function settleBrowserObservation(initial, call) {
  let response = initial, data = dataOf(initial), image = imageOf(initial);
  let imageObservationId = image ? data?.observation_id : null;
  let imageObservedAt = image ? data?.observed_at : null;
  let error = null;
  const observations = [];
  for (let attempt = 0; attempt < 4 && loading.has(data?.page_status); attempt++) {
    let next;
    try {
      next = await call('devbox_browser', {
        action: 'observe', session_id: data.session_id, wait_ms: 1500, include_html: true,
      });
    } catch (failure) {
      error = String(failure);
      observations.push({ ok: false, error });
      break;
    }
    const nextData = dataOf(next);
    observations.push({ ok: !next.isError, status: nextData?.page_status, observed_at: nextData?.observed_at });
    if (next.isError) {
      error = next.structuredContent?.summary ?? 'Browser observation failed';
      break;
    }
    response = next;
    data = nextData;
    const nextImage = imageOf(next);
    if (nextImage) {
      image = nextImage;
      imageObservationId = data?.observation_id;
      imageObservedAt = data?.observed_at;
    }
  }
  return { response, data, image, imageObservationId, imageObservedAt, observations, error };
}
