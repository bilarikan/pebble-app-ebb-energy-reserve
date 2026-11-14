// Settings page layout for pebble-clay (PRD FR10).
module.exports = [
  {
    type: 'heading',
    defaultValue: 'Ebb'
  },
  {
    type: 'text',
    defaultValue: 'The score is an estimate computed on your watch. It is not medical advice. ' +
      'Nothing on this page is sent anywhere except to your watch.'
  },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Sampling' },
      {
        type: 'select',
        messageKey: 'Intensity',
        label: 'Sampling intensity',
        description: 'How often the heart-rate sensor is held on to measure HRV. ' +
          'More samples steady the score and cost more battery.',
        defaultValue: '1',
        options: [
          { label: 'Low: every 20 min asleep, 60 min awake', value: '0' },
          { label: 'Balanced: every 10 min asleep, 30 min awake', value: '1' },
          { label: 'High: every 5 min asleep, 15 min awake', value: '2' }
        ]
      }
    ]
  },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Display and model' },
      {
        type: 'select',
        messageKey: 'HrvUnits',
        label: 'HRV units',
        defaultValue: '0',
        options: [
          { label: 'RMSSD in milliseconds', value: '0' },
          { label: 'ln RMSSD', value: '1' }
        ]
      },
      {
        type: 'slider',
        messageKey: 'MaxHr',
        label: 'Maximum heart rate (bpm)',
        description: 'Used to judge how hard an activity is. 220 minus your age is a rough guide.',
        defaultValue: 185,
        min: 140,
        max: 210,
        step: 1
      }
    ]
  },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Baseline' },
      {
        type: 'toggle',
        messageKey: 'ResetBaseline',
        label: 'Reset baseline on save',
        description: 'Forgets your overnight HRV and resting heart-rate history on the watch ' +
          'and the backup on this phone. The app recalibrates over the next three nights.',
        defaultValue: false
      }
    ]
  },
  {
    type: 'submit',
    defaultValue: 'Save'
  }
];
