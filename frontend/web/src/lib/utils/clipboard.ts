/** Is there a clipboard to write to at all? For deciding whether to OFFER a copy: a button
 * that can only disappoint is worse than no button. A caller that merely acts on a click
 * does not need this — copyText asks the same question on the way in and says so. */
export function clipboardAvailable(): boolean {
  return typeof navigator !== "undefined" && typeof navigator.clipboard?.writeText === "function";
}

// One answer to "did the text actually reach the clipboard". A clipboard write changes
// nothing on screen, so it is the one action whose outcome a caller cannot infer — a
// swallowed failure leaves the user believing they are holding text they are not.
//
// Reports the reason rather than a bare boolean so the caller can say WHY in the same
// sentence it says the copy failed, and answers on the unavailable path too: CEF serves
// this app from a custom scheme, where the API is present but may refuse.
export async function copyText(text: string): Promise<string | null> {
  if (!clipboardAvailable()) {
    return "the clipboard is unavailable here";
  }
  try {
    await navigator.clipboard.writeText(text);
    return null;
  } catch (e) {
    return (e as Error).message || "the clipboard refused the write";
  }
}
