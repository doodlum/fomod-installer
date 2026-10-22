#if NETFRAMEWORK
// .NET Framework 4.8 (Mono) lacks the GetValueOrDefault that newer frameworks provide
namespace System.Collections.Generic {
    internal static class FrameworkDictionaryExtensions {
        public static TValue GetValueOrDefault<TKey,TValue>(this IDictionary<TKey,TValue> dictionary, TKey key) {
            TValue value;
            return dictionary.TryGetValue(key, out value) ? value : default(TValue);
        }
    }
}
#endif
