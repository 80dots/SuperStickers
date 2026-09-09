// 내려받을 수 있는 Ollama 모델: 엄선된 고정 목록. 설정 화면과 AI 설정 마법사가 함께 쓴다.
// size는 안내용 대략치(Ollama 태그 기준). 정확한 크기는 내려받으며 진행률로 보여 준다.
const ollamaModels = (() => {
  const LIST = [
    { name: 'gemma3:4b',   size: '3.3 GB',  note: 'light' },
    { name: 'qwen3.5:4b',  size: '2.6 GB',  note: 'light' },
    { name: 'llama3:8b',   size: '4.7 GB',  note: 'balanced' },
    { name: 'qwen3.5:9b',  size: '5.6 GB',  note: 'balanced' },
    { name: 'gemma3:12b',  size: '8.1 GB',  note: 'quality' },
    { name: 'gemma4:12b',  size: '9.6 GB',  note: 'quality' },
    { name: 'gpt-oss:20b', size: '14 GB',   note: 'quality', recommended: true },
  ];
  return {
    list: () => LIST.slice(),
    names: () => LIST.map((m) => m.name),
    // 태그가 붙은 이름("llama3:8b")과 붙지 않은 이름("llama3")을 같은 것으로 본다
    find: (name) => LIST.find((m) => m.name === name) || null,
  };
})();
